/* src/ops/agg_engine.c — v2 aggregation engine. */
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#  define _GNU_SOURCE       /* pread, pwrite, O_CLOEXEC: the radix spill file */
#endif
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "ops/hash.h"     /* ray_hash_bytes — wide (STR) group-key hashing */
#include "ops/internal.h"  /* col_vec_new, col_esz */
#include "ops/colbind.h"
#include "ops/colgather.h"  /* generation-2 compact gathers */
#include "ops/rowsel.h"    /* ray_rowsel_meta, ray_rowsel_to_indices */
#include "lang/internal.h" /* sym_domain_rep */
#include "table/domain.h"
#include "table/table.h"
#include "table/sym.h"    /* ray_read_sym */
#include "store/col.h"    /* ray_col_cold — sampled-row read-ahead */
#include "core/pool.h"     /* ray_pool_want — a column asked for whole */
#include "core/platform.h" /* ray_cache_llc_bytes — replicated slab bound */
#include "store/enc.h"      /* RAY_ENC_CHUNK: directory statistics per chunk */
#include "store/enccol.h"   /* ray_enc_chunk_stat_t */
#include "store/enceval.h"  /* ray_enc_runs: COUNT groups on key runs */
#include <stdlib.h>
#include <math.h>
#include <string.h>

/* The radix spill file (agg_radix_spill_t) is POSIX pread/pwrite; Windows
 * keeps the re-scanned waves only, as its heap keeps anonymous pools only. */
#if defined(RAY_OS_WINDOWS)
#  define AGG_RADIX_FILE_SPILL 0
#else
#  define AGG_RADIX_FILE_SPILL 1
#  include <errno.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

/* Radix output address: high 32 bits partition, low 32 bits local group. */
typedef struct { int64_t idx; } agg_radix_order_t;

bool ray_agg_engine_v2 = true;   /* knob; default on */
bool ray_agg_tablestack = true;  /* knob; default on */
int64_t ray_agg_radix_wave_bytes = 0;   /* knob; 0 = heap-derived wave bound, <0 = off */
int ray_agg_radix_spill = 0;            /* knob; 0 = by bytes, 1 = never, 2 = always, 3..6 = always with a fault */
static bool agg_force_smallhash;    /* test hook (agg_test_force_smallhash) */
static bool agg_no_wide_dense;      /* test hook (agg_test_no_wide_dense) */
void agg_test_force_smallhash(bool on) { agg_force_smallhash = on; }
void agg_test_no_wide_dense(bool on) { agg_no_wide_dense = on; }

static _Thread_local agg_route_stats_t route_stats;
void agg_route_reset(void) { memset(&route_stats, 0, sizeof(route_stats)); }
agg_route_stats_t agg_route_stats(void) { return route_stats; }
void agg_route_note_key_domain(void) { route_stats.key_domain_evals++; }
void agg_route_note_vocab_order(void) { route_stats.key_vocab_order++; }
void agg_route_note_da_lazy(uint64_t skipped_chunks) {
    route_stats.da_lazy_merges++;
    route_stats.da_lazy_skipped += skipped_chunks;
}
void agg_route_record(agg_route_t route) {
    static const char* const names[AGG_ROUTE_COUNT] = {
        "group: none", "group: legacy", "group: slices", "group: parted",
        "group: v2 serial dense", "group: v2 serial hash", "group: v2 dense",
        "group: v2 radix", "group: v2 hash", "group: v2 smallhash", "group: v2 indexed",
        "group: v2 tablestack"
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
    if (op == OP_MEDIAN || op == OP_QUANTILE) return number || t == RAY_TIMESPAN;
    if (op == OP_MODE || op == OP_TOP_N || op == OP_BOT_N) return scalar;
    return false;
}

/* A column's metadata as the v2 engine routes it: under an in-place concat
 * group (g->grp_seg_stream) a parted column reads as its base type, since
 * its sources are read from the segments; otherwise unchanged. */
static bool agg_col_meta(ray_graph_t* g, ray_t* tbl, int64_t sym, ray_col_meta_t* m) {
    if (!ray_table_col_meta(tbl, sym, m)) return false;
    if (g->grp_seg_stream && RAY_IS_PARTED(m->type)) m->type = (int8_t)RAY_PARTED_BASETYPE(m->type);
    return true;
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
            ray_col_meta_t cm;
            /* agg_col_meta: a parted _count (an in-place concat's probe) is
             * its base I64, so the shortcut keeps the group as over the flatten */
            if (agg_col_meta(g, tbl, ray_sym_intern("_count", 6), &cm) && cm.type == RAY_I64) return AGG_V2_SHAPE;
        }
    }

    /* every key must be a plain column scan of a supported type */
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_op_t* key = op_node(g, ext->keys[k]);
        if (!key || key->opcode != OP_SCAN) return AGG_V2_KEY_EXPRESSION;
        ray_op_ext_t* kext = find_ext(g, key->id);
        ray_col_meta_t km;
        if (!kext || !agg_col_meta(g, tbl, kext->sym, &km)) return AGG_V2_SHAPE;
        switch (km.type) {
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
            ray_col_meta_t cm;
            if (!agg_col_meta(g, tbl, ke->sym, &cm)) continue;   /* present: the key loop above checked */
            if (cm.type == RAY_F32 || cm.type == RAY_F64) floating = true;
            if (cm.type == RAY_STR || cm.type == RAY_GUID || cm.type == RAY_LIST) indexed = true;
        }
        for (uint32_t a = 0; a < ext->n_aggs; a++) {
            ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
            ray_col_meta_t cm;
            if (ie && agg_col_meta(g, tbl, ie->sym, &cm) && agg_indexed_supported(ext->agg_ops[a], cm.type)) indexed = true;
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
            ray_col_meta_t im;
            bool ih = ie && agg_col_meta(g, tbl, ie->sym, &im);
            if (ih && agg_indexed_supported(ext->agg_ops[a], im.type)) continue;
            const agg_vtable_t* vt = ih ? agg_resolve(ext->agg_ops[a], im.type) : NULL;
            if (!vt) return AGG_V2_AGG_TYPE;
            if (vt->kind != ACC_STREAMING) return AGG_V2_BUFFERED;
            continue;  /* admitted */
        }
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {
            if (!agg_is_binary_agg(ext->agg_ops[a])) return AGG_V2_SHAPE;
            ray_op_t* xin = op_node(g, ext->agg_ins[a]); ray_op_t* yin = op_node(g, ext->agg_ins2[a]);
            if (!xin || xin->opcode != OP_SCAN || !yin || yin->opcode != OP_SCAN) return AGG_V2_AGG_EXPRESSION;
            ray_op_ext_t* xe = find_ext(g, xin->id); ray_op_ext_t* ye = find_ext(g, yin->id);
            ray_col_meta_t xm, ym;
            if (!xe || !ye || !agg_col_meta(g, tbl, xe->sym, &xm)
                || !agg_col_meta(g, tbl, ye->sym, &ym)) return AGG_V2_SHAPE;
            if (!agg_resolve(ext->agg_ops[a], xm.type)) return AGG_V2_AGG_TYPE;
            if (!agg_resolve(ext->agg_ops[a], ym.type)) return AGG_V2_AGG_TYPE;
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
        ray_col_meta_t im;
        if (!ie || !agg_col_meta(g, tbl, ie->sym, &im)) return AGG_V2_SHAPE;
        if (agg_indexed_supported(ext->agg_ops[a], im.type)) continue;
        const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], im.type);
        if (!vt) return AGG_V2_AGG_TYPE;
        if (vt->kind != ACC_STREAMING) return AGG_V2_BUFFERED;
        /* The streaming 64-bit integer avg packs its 128-bit high word with
         * the group count in one word (agg_stream.c): exact while every
         * group has fewer than 2^32 rows; the narrower kernels keep a plain
         * int64 sum, exact below 2^31 rows.  A table that large keeps the
         * legacy engines, whose accumulators carry a full high word. */
        if (ext->agg_ops[a] == OP_AVG && im.type != RAY_F64 && im.type != RAY_F32 &&
            ray_table_nrows(tbl) >= ((int64_t)1 << ((im.type == RAY_I64 || im.type == RAY_TIMESTAMP) ? 32 : 31)))
            return AGG_V2_AGG_TYPE;
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

/* ── Range reads: generation-2 sources ───────────────────────────────────
 * A strategy's sources are its keys, then its aggregate inputs, then the
 * y-sides of binary aggregates (n_keys + 2 * n_aggs).  A plain source (a
 * generation-0 column, a published materialization) is read in place
 * through its row-0 pointer.  A staged source (generation 2) is read by
 * range: the task body runs over pieces of its rows (R-WALK) or over runs
 * of ascending rows (R-SPARSE), and reads every column at row - base
 * through a view.  A plain context runs today's loop over its whole-column
 * view (base 0) and never calls the walk. */
typedef struct {
    ray_col_src_t* const* srcs;      /* [n_src] staged sources; NULL: read in place (or COUNT) */
    const void* const*    plain;     /* [n_src] row-0 pointers; NULL for staged sources and COUNT */
    const uint8_t*        esz;       /* [n_src] element widths (keys: the column's SYM width) */
    const int8_t*         key_types; /* [n_keys] */
    const uint8_t*        key_attrs; /* [n_keys] the plain columns' (stand-ins') attrs */
    uint32_t              n_src, n_keys, n_aggs;
    ray_scan_fail_t*      fail;
    bool                  staged;    /* some source is staged */
} agg_rr_t;

/* What a task body reads rows through: element 0 of every pointer is row
 * `base`.  The whole-column view has base 0; a piece's view is positioned
 * at the piece's first row and carries the pieces' SYM widths. */
typedef struct {
    const void* const* key_data;    /* [n_keys] */
    const uint8_t*     key_attrs;   /* [n_keys] SYM widths of key_data */
    const void* const* val_data;    /* [n_aggs]; NULL: no input (COUNT) */
    const void* const* val2_data;   /* [n_aggs] y-sides of binary aggregates */
    int64_t            base;
} agg_view_t;

static inline agg_view_t agg_view_whole(const agg_rr_t* rr) {
    return (agg_view_t){ .key_data = rr->plain, .key_attrs = rr->key_attrs,
                         .val_data = rr->plain + rr->n_keys,
                         .val2_data = rr->plain + rr->n_keys + rr->n_aggs, .base = 0 };
}

/* Key k of view v at absolute row r. */
static inline int64_t agg_vkey(const agg_view_t* v, const int8_t* key_types, uint32_t k, int64_t r) {
    return agg_read_key_i64_raw(key_types[k], v->key_attrs[k], v->key_data[k], r - v->base);
}

/* A body over rows [start, end) (or over indices [start, end) of an index
 * list, for the R-SPARSE driver) read through view v. */
typedef void (*agg_body_fn)(void* ctx, const agg_view_t* v, uint32_t wid, int64_t start, int64_t end);

/* The view of row b: staged sources from their pieces or views (ptr/kat
 * already filled for them), the others offset from their row-0 pointers. */
static void agg_view_set(const agg_rr_t* rr, int64_t b, const void** ptr, uint8_t* kat,
                         agg_view_t* v) {
    for (uint32_t i = 0; i < rr->n_src; i++)
        if (!rr->srcs[i]) ptr[i] = rr->plain[i] ? (const char*)rr->plain[i] + (size_t)b * rr->esz[i] : NULL;
    for (uint32_t k = 0; k < rr->n_keys; k++)
        if (!rr->srcs[k] || rr->key_types[k] != RAY_SYM) kat[k] = rr->key_attrs[k];
    v->key_data = ptr;
    v->key_attrs = kat;
    v->val_data = ptr + rr->n_keys;
    v->val2_data = ptr + rr->n_keys + rr->n_aggs;
    v->base = b;
}

typedef struct {
    void*           ctx;
    agg_body_fn     body;
    const agg_rr_t* rr;
} agg_piece_ctx_t;

static void agg_piece(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    agg_piece_ctx_t* pc = (agg_piece_ctx_t*)ctx;
    const agg_rr_t* rr = pc->rr;
    uint32_t ns = rr->n_src ? rr->n_src : 1, nk = rr->n_keys ? rr->n_keys : 1;
    const void* ptr[ns];
    uint8_t kat[nk];
    for (uint32_t i = 0; i < rr->n_src; i++)
        if (rr->srcs[i]) ptr[i] = p[i].data;
    for (uint32_t k = 0; k < rr->n_keys; k++)
        if (rr->srcs[k]) kat[k] = p[k].attrs;
    agg_view_t v;
    agg_view_set(rr, b, ptr, kat, &v);
    pc->body(pc->ctx, &v, wid, b, e);
}

/* R-WALK of rows [start, end) of a staged context.  A range error is noted
 * keyed by `key` (the task's first row or position) and false returned; a
 * task below a noted failure still runs (it may fail lower). */
static bool agg_walk(const agg_rr_t* rr, void* ctx, agg_body_fn body,
                     uint32_t wid, int64_t start, int64_t end, int64_t key) {
    if (ray_scan_fail_skip(rr->fail, key)) return false;
    agg_piece_ctx_t pc = { ctx, body, rr };
    ray_t* err = ray_col_walk(rr->srcs, rr->plain, rr->n_src, start, end, false, &pc, wid, agg_piece);
    if (!err) return true;
    ray_scan_fail_note(rr->fail, key, err);
    return false;
}

/* One cursor per staged source, created by a task at its start and freed
 * at its end (R-SPARSE: each touched vector is decoded once per task).  Up
 * to AGG_CURS_STACK cursors live in the set, on the task's stack, and the
 * first AGG_CURS_LENT encoded sources decode into fixed vectors lent from
 * the set, so a worker's first decode takes nothing from its heap (a fresh
 * worker heap maps and faults in a whole pool on its first allocation:
 * ClickBench q8 on partitions, first run).  Two, not one per cursor: the
 * set's frame is probed page by page on every task's first entry, and
 * eight lent vectors (128 KB) measured q8's generation-0 first run 3%
 * slower.  Declared without an initializer (the fixed vectors are not to
 * be zeroed): set cur = NULL before any path that may free it. */
enum { AGG_CURS_STACK = 8, AGG_CURS_LENT = 2 };
typedef struct {
    ray_col_cursor_t  st[AGG_CURS_STACK];
    ray_col_fixed_t   fx[AGG_CURS_LENT];
    ray_col_cursor_t* cur;
} agg_curs_t;

static bool agg_curs_init(agg_curs_t* cs, const agg_rr_t* rr) {
    cs->cur = rr->n_src <= AGG_CURS_STACK ? cs->st
            : ray_calloc_raw((size_t)rr->n_src * sizeof(ray_col_cursor_t));
    if (!cs->cur) return false;
    uint32_t lent = 0;
    for (uint32_t i = 0; i < rr->n_src; i++) {
        if (!rr->srcs[i]) continue;
        if (lent < AGG_CURS_LENT && ray_col_src_encoded(rr->srcs[i]))
            ray_col_cursor_init_lent(&cs->cur[i], rr->srcs[i], &cs->fx[lent++]);
        else
            ray_col_cursor_init(&cs->cur[i], rr->srcs[i]);
    }
    return true;
}

static void agg_curs_free(agg_curs_t* cs, const agg_rr_t* rr) {
    if (!cs->cur) return;
    for (uint32_t i = 0; i < rr->n_src; i++)
        if (rr->srcs[i]) ray_col_cursor_free(&cs->cur[i]);
    if (cs->cur != cs->st) ray_free_raw(cs->cur);
    cs->cur = NULL;
}

/* agg_walk through the task's cursors cs (agg_curs_init), which persist
 * across its calls: a task walking consecutive blocks decodes the vector
 * one block ends inside once, not once per block. */
static bool agg_walk_cs(const agg_rr_t* rr, agg_curs_t* cs, void* ctx, agg_body_fn body,
                        uint32_t wid, int64_t start, int64_t end, int64_t key) {
    if (ray_scan_fail_skip(rr->fail, key)) return false;
    agg_piece_ctx_t pc = { ctx, body, rr };
    ray_t* err = ray_col_walk_cur(rr->srcs, rr->plain, rr->n_src, cs->cur, start, end, &pc, wid, agg_piece);
    if (!err) return true;
    ray_scan_fail_note(rr->fail, key, err);
    return false;
}

/* Point reads of a staged key or input (ray_col_src_points_i64's
 * contract: the ascending rows[0, n) of one vector, 1 / 0 / -1).  A SYM
 * source's rows are its ids (ray_col_src_gather_points: each checked
 * against the domain bound, id 0 the in-band null served as itself), so
 * an id key costs no vector decode either.  SYM has no null bitmap; a
 * flagged row would be one, and declines (the cursor reads it). */
static inline int agg_src_points(ray_col_src_t* s, const int64_t* rows, int64_t n, int64_t* out, ray_t** err) {
    if (s->type != RAY_SYM) return ray_col_src_points_i64(s, rows, n, out, err);
    *err = NULL;
    if (n < 1 || n > RAY_COL_POINT_ROWS) return 0;
    uint8_t nul[RAY_COL_POINT_ROWS];
    int pr = ray_col_src_gather_points(s, rows, n, out, nul, err);
    if (pr > 0) for (int64_t q = 0; q < n; q++) if (nul[q]) return 0;
    return pr;
}

/* R-SPARSE over the ascending rows idx[i0, i1) through the task's cursors:
 * the rows every staged source's current view covers form one run, handed
 * to body as the index range [ib, ie) with a view positioned at idx[ib].
 * Point reads (open decision D2): the first AGG_CURS_LENT integer and SYM sources
 * read a vector holding few of the rows (D2, per codec; none
 * decoded yet) by point reads into a vector-sized buffer of the walk, at
 * the rows' own offsets: a sparse view, valid for exactly those rows,
 * which are every row of idx[i0, i1) inside the vector, so no run reads
 * another.  A declined vector is read through the cursor.
 * NULL or an owned error (body is not called again after an error). */
static ray_t* agg_idx_walk(const agg_rr_t* rr, agg_curs_t* cs, const int64_t* idx,
                           int64_t i0, int64_t i1, void* ctx, agg_body_fn body, uint32_t wid) {
    uint32_t ns = rr->n_src ? rr->n_src : 1, nk = rr->n_keys ? rr->n_keys : 1;
    const void* ptr[ns];
    uint8_t kat[nk];
    /* the sparse views: their sources, vectors [lo, hi) and buffers */
    _Alignas(64) int64_t spb[AGG_CURS_LENT][RAY_MORSEL_ELEMS];
    int64_t sp_lo[AGG_CURS_LENT], sp_hi[AGG_CURS_LENT];
    uint32_t sp_src[AGG_CURS_LENT], nsp = 0;
    for (uint32_t s = 0; s < rr->n_src && nsp < AGG_CURS_LENT; s++) {
        ray_col_src_t* src = rr->srcs[s];
        if (!src || !ray_col_src_encoded(src)) continue;
        switch (src->type) {
        case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8: case RAY_BOOL:
        case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_SYM:
            sp_src[nsp] = s; sp_lo[nsp] = sp_hi[nsp] = -1; nsp++; break;
        default: break;
        }
    }
    for (int64_t i = i0; i < i1; ) {
        int64_t b = idx[i], e = INT64_MAX;
        for (uint32_t s = 0, sp = 0; s < rr->n_src; s++) {
            if (!rr->srcs[s]) continue;
            int slot = -1;
            if (sp < nsp && sp_src[sp] == s) slot = (int)sp++;
            if (slot >= 0) {
                ray_col_cursor_t* c = &cs->cur[s];
                bool covered = c->have && b >= c->v.start && b < c->v.start + c->v.n;
                int64_t vlo, vhi;
                if (!covered && !(b >= sp_lo[slot] && b < sp_hi[slot]) &&
                    ray_col_src_vec_span(rr->srcs[s], b, &vlo, &vhi)) {
                    int64_t j = i + 1;
                    while (j < i1 && j - i <= RAY_COL_POINT_ROWS && idx[j] < vhi) j++;
                    if (j - i <= RAY_COL_POINT_ROWS) {
                        int64_t x[RAY_COL_POINT_ROWS];
                        ray_t* err = NULL;
                        int pr = agg_src_points(rr->srcs[s], idx + i, j - i, x, &err);
                        if (pr < 0) return err;
                        if (pr > 0) {
                            uint8_t esz = ray_sym_elem_size(rr->srcs[s]->type, rr->srcs[s]->attrs);
                            char* d = (char*)spb[slot];
                            for (int64_t q = 0; q < j - i; q++) {
                                size_t at = (size_t)(idx[i + q] - vlo) * esz;
                                switch (esz) {
                                case 1:  d[at] = (char)x[q]; break;
                                case 2:  { int16_t y = (int16_t)x[q]; memcpy(d + at, &y, 2); break; }
                                case 4:  { int32_t y = (int32_t)x[q]; memcpy(d + at, &y, 4); break; }
                                default: memcpy(d + at, &x[q], 8); break;
                                }
                            }
                            sp_lo[slot] = vlo; sp_hi[slot] = vhi;
                        }
                    }
                }
                if (!covered && b >= sp_lo[slot] && b < sp_hi[slot]) {
                    uint8_t esz = ray_sym_elem_size(rr->srcs[s]->type, rr->srcs[s]->attrs);
                    ptr[s] = (const char*)spb[slot] + (size_t)(b - sp_lo[slot]) * esz;
                    if (s < rr->n_keys) kat[s] = rr->srcs[s]->attrs;
                    if (sp_hi[slot] < e) e = sp_hi[slot];
                    continue;
                }
            }
            const ray_col_range_t* v;
            ray_t* err = ray_col_cursor_at(&cs->cur[s], b, &v);
            if (err) return err;
            ptr[s] = (const char*)v->data + (size_t)(b - v->start) * ray_sym_elem_size(v->type, v->attrs);
            if (s < rr->n_keys) kat[s] = v->attrs;
            if (v->start + v->n < e) e = v->start + v->n;
        }
        int64_t j = i + 1;
        while (j < i1 && idx[j] < e) j++;
        agg_view_t view;
        agg_view_set(rr, b, ptr, kat, &view);
        body(ctx, &view, wid, i, j);
        i = j;
    }
    return NULL;
}

/* Single-row reads for the samplers (strided, sparse): in place, or
 * from a staged source's packed vector (a point read, no vector decode),
 * else through its cursor.  *err is set on a range error. */
static inline int64_t agg_key_at(const agg_rr_t* rr, agg_curs_t* cs, uint32_t k, int64_t r, ray_t** err) {
    if (!rr->srcs[k]) return agg_read_key_i64_raw(rr->key_types[k], rr->key_attrs[k], rr->plain[k], r);
    int64_t x;
    int pr = rr->srcs[k]->type == RAY_SYM ? agg_src_points(rr->srcs[k], &r, 1, &x, err)
                                          : ray_col_src_point_i64(rr->srcs[k], r, &x, err);
    if (pr) return pr > 0 ? x : 0;
    const ray_col_range_t* v;
    if ((*err = ray_col_cursor_at(&cs->cur[k], r, &v)) != NULL) return 0;
    return agg_read_key_i64_raw(rr->key_types[k], v->attrs, v->data, r - v->start);
}
static inline const void* agg_elem_at(const agg_rr_t* rr, agg_curs_t* cs, uint32_t i, int64_t r, ray_t** err) {
    if (!rr->srcs[i]) return rr->plain[i] ? (const char*)rr->plain[i] + (size_t)r * rr->esz[i] : NULL;
    const ray_col_range_t* v;
    if ((*err = ray_col_cursor_at(&cs->cur[i], r, &v)) != NULL) return NULL;
    return (const char*)v->data + (size_t)(r - v->start) * rr->esz[i];
}

/* A zero-length column of a staged source's type standing in for it where
 * a strategy reads the column's type, SYM width, domain and nullability
 * (plans, layouts, emits); NULL on OOM.  Its data is never read. */
static ray_t* agg_src_standin(const ray_col_src_t* s) {
    /* a parted SYM source's own width is not authoritative (segments may
     * differ): the stand-in takes the widest segment's, which plans and
     * emits size their slots and output by */
    uint8_t w = s->parted ? parted_sym_width(s->obj) : (s->attrs & RAY_SYM_W_MASK);
    ray_t* v = s->type == RAY_SYM ? ray_sym_vec_new(w, 0)
                                  : ray_vec_new(s->type, 0);
    if (!v || RAY_IS_ERR(v)) {
        if (v) ray_error_free(v);
        return NULL;
    }
    if (s->type == RAY_SYM && s->dom && s->dom != ray_sym_runtime_domain()) {
        ray_sym_domain_retain(s->dom);
        v->sym_domain = s->dom;
    }
    if (s->may_null && s->type != RAY_SYM) v->attrs |= RAY_ATTR_HAS_NULLS;
    v->len = 0;
    return v;
}

/* The keys of one group run: bound once by exec_group_v2_run_inner.  A
 * staged key's column is a stand-in (agg_src_standin). */
typedef struct {
    agg_rr_t        rr;          /* keys only (n_aggs 0) */
    ray_col_src_t** srcs;        /* [n] staged keys' sources */
    const void**    plain;       /* [n] */
    int8_t*         types;
    uint8_t*        attrs;
    uint8_t*        esz;
    ray_scan_fail_t fail;
} agg_keyset_t;

/* R-RANDOM: every staged key takes its plain column (a generation-2 key
 * decodes once, through its once-state), for a strategy that reads keys at
 * stored rows or whole.  NULL or an owned error. */
static ray_t* agg_keys_unstage(ray_t** key_cols, agg_keyset_t* ks) {
    for (uint32_t k = 0; k < ks->rr.n_keys; k++) {
        if (!ks->srcs[k]) continue;
        ray_t* plain = NULL;
        ray_t* err = ray_col_src_plain(ks->srcs[k], &plain);   /* random: unstage: v2 whole-column strategy */
        if (err) return err;
        if (!plain) return ray_error("domain", "group key: no column");
        key_cols[k] = plain;               /* borrowed: valid while the source is bound */
        ks->plain[k] = ray_data(plain);
        ks->attrs[k] = plain->attrs;
        ks->esz[k] = ray_sym_elem_size(plain->type, plain->attrs);
        ks->srcs[k] = NULL;
    }
    ks->rr.staged = false;
    return NULL;
}

/* Hoist type and null handling out of range scans so native integer
 * reductions can vectorize, including nullable temporal keys. */
static void agg_key_bounds_raw(int8_t type, uint8_t attrs, const void* data,
                               int64_t start, int64_t end, bool nullable,
                               int64_t null, int64_t* lo, int64_t* hi) {
    int64_t mn = INT64_MAX, mx = INT64_MIN;
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
    switch (type) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: KEY_BOUNDS(int64_t); break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: KEY_BOUNDS(int32_t); break;
        case RAY_I16: KEY_BOUNDS(int16_t); break;
        case RAY_U8: case RAY_BOOL: KEY_BOUNDS(uint8_t); break;
        case RAY_SYM:
            switch (attrs & RAY_SYM_W_MASK) {
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
static void agg_key_bounds(ray_t* key, int64_t start, int64_t end, bool nullable,
                           int64_t null, int64_t* lo, int64_t* hi) {
    agg_key_bounds_raw(key->type, key->attrs, ray_data(key), start, end, nullable, null, lo, hi);
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
        /* Often the first read of the key: read ahead block by block.  The
         * key is the one column read, so it is requested from the first
         * block on. */
        int64_t lo = INT64_MAX, hi = INT64_MIN;
        for (int64_t b = begin; b < limit; b += 65536) {
            int64_t e = limit - b > 65536 ? b + 65536 : limit, l, h;
            ray_pool_scan_col_at(ray_data(c->key), c->rows, b, limit);
            agg_key_bounds(c->key, b, e, c->nullable, c->null, &l, &h);
            if (l < lo) lo = l;
            if (h > hi) hi = h;
        }
        c->bounds[task * 2] = lo;
        c->bounds[task * 2 + 1] = hi;
    }
}

/* A staged key's prescan: one task per scanned span (a chunk without
 * directory statistics, or the whole key when it has no directory), each
 * span read by pieces (R-WALK) into its task's bounds. */
typedef struct {
    const agg_rr_t* rr;          /* the key alone */
    bool nullable;
    int64_t null;
    const int64_t* spans;        /* [tasks][2] row ranges */
    int64_t* bounds;             /* [tasks][2] */
} agg_key_scan_ctx_t;
typedef struct { const agg_key_scan_ctx_t* c; int64_t lo, hi; } agg_key_scan_task_t;
static void agg_key_scan_piece(void* raw, const agg_view_t* v, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_key_scan_task_t* t = raw;
    int64_t lo, hi;
    agg_key_bounds_raw(t->c->rr->key_types[0], v->key_attrs[0], v->key_data[0],
                       0, end - start, t->c->nullable, t->c->null, &lo, &hi);
    if (lo < t->lo) t->lo = lo;
    if (hi > t->hi) t->hi = hi;
}
static void agg_key_scan_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    agg_key_scan_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        agg_key_scan_task_t t = { c, INT64_MAX, INT64_MIN };
        (void)agg_walk(c->rr, &t, agg_key_scan_piece, wid, c->spans[task * 2], c->spans[task * 2 + 1],
                       c->spans[task * 2]);
        c->bounds[task * 2] = t.lo; c->bounds[task * 2 + 1] = t.hi;
    }
}

/* One chunk of a parted key (ray_col_src_parted_chunks): folded from its
 * segment's statistics as agg_key_bounds_staged folds a directory's, or
 * appended to the spans to scan (counted only while spans is NULL). */
typedef struct {
    int8_t   type;
    int64_t  mn, mx;
    int64_t* spans;
    int64_t  n;
} agg_bounds_fold_t;
static void agg_bounds_chunk(void* raw, int64_t lo, int64_t hi, const ray_enc_chunk_stat_t* st) {
    agg_bounds_fold_t* f = raw;
    if (st) {
        if (f->type == RAY_SYM && st->null_count > 0 && f->mn > 0) f->mn = 0;
        if ((int64_t)st->null_count >= st->rows) return;   /* all null */
        if (st->has_minmax) {
            if (st->min < f->mn) f->mn = st->min;
            if (st->max > f->mx) f->mx = st->max;
            return;
        }
    }
    if (f->spans) { f->spans[f->n * 2] = lo; f->spans[f->n * 2 + 1] = hi; }
    f->n++;
}

/* Bounds of a staged key from its directory: per chunk min / max, all-null
 * chunks skipped (a SYM chunk with nulls also spans id 0, the null symbol);
 * chunks without statistics, and keys without a directory, are scanned by
 * pieces.  A parted key folds its encoded segments' chunks the same way
 * (ray_col_src_parted_chunks) and scans its plain ones.  NULL or an owned
 * error. */
static ray_t* agg_key_bounds_staged(const agg_rr_t* krr, uint32_t k, int64_t rows, bool nullable,
                                    int64_t null, int64_t* lo, int64_t* hi) {
    ray_col_src_t* src = krr->srcs[k];
    int64_t mn = INT64_MAX, mx = INT64_MIN;
    int64_t nch = (rows + RAY_ENC_CHUNK - 1) / RAY_ENC_CHUNK;
    ray_enc_chunk_stat_t st;
    bool dir = nch > 0 && ray_col_src_chunk_stat(src, 0, &st);
    ray_t* hdr = NULL;
    int64_t* spans;
    int64_t* bounds;
    int64_t n = 0;
    agg_bounds_fold_t pf = { krr->key_types[k], INT64_MAX, INT64_MIN, NULL, 0 };
    ray_t* perr = NULL;
    if (src->parted && ray_col_src_encoded(src) && ray_col_src_parted_chunks(src, &pf, agg_bounds_chunk, &perr)) {
        /* the parted directory: count the chunks to scan, then list them */
        int64_t ns = pf.n;
        spans = (int64_t*)scratch_alloc(&hdr, (size_t)(ns > 0 ? ns : 1) * 4 * sizeof(int64_t));
        if (!spans) return ray_error("oom", NULL);
        bounds = spans + (ns > 0 ? ns : 1) * 2;
        pf = (agg_bounds_fold_t){ krr->key_types[k], INT64_MAX, INT64_MIN, spans, 0 };
        (void)ray_col_src_parted_chunks(src, &pf, agg_bounds_chunk, &perr);
        mn = pf.mn; mx = pf.mx; n = pf.n;
        nch = 0;
    } else {
        if (perr) return perr;
        spans = (int64_t*)scratch_alloc(&hdr, (size_t)(nch > 0 ? nch : 1) * 4 * sizeof(int64_t));
        if (!spans) return ray_error("oom", NULL);
        bounds = spans + (nch > 0 ? nch : 1) * 2;
    }
    for (int64_t ch = 0; ch < nch; ch++) {
        int64_t b = ch * RAY_ENC_CHUNK, e = b + RAY_ENC_CHUNK < rows ? b + RAY_ENC_CHUNK : rows;
        if (dir) {
            (void)ray_col_src_chunk_stat(src, ch, &st);
            if (krr->key_types[k] == RAY_SYM && st.null_count > 0 && mn > 0) mn = 0;
            if ((int64_t)st.null_count >= st.rows) continue;   /* all null */
            if (st.has_minmax) {
                if (st.min < mn) mn = st.min;
                if (st.max > mx) mx = st.max;
                continue;
            }
        }
        spans[n * 2] = b; spans[n * 2 + 1] = e; n++;
    }
    ray_t* err = NULL;
    if (n > 0) {
        ray_col_src_t* one_src[1] = { src };
        const void* one_plain[1] = { NULL };
        uint8_t one_esz[1] = { krr->esz[k] };
        ray_scan_fail_t fail;
        ray_scan_fail_init(&fail);
        agg_rr_t rr = { .srcs = one_src, .plain = one_plain, .esz = one_esz,
            .key_types = krr->key_types + k, .key_attrs = krr->key_attrs + k,
            .n_src = 1, .n_keys = 1, .n_aggs = 0, .fail = &fail, .staged = true };
        agg_key_scan_ctx_t c = { &rr, nullable, null, spans, bounds };
        ray_col_src_advise(src, 0, rows);
        ray_pool_t* pool = ray_pool_get();
        bool par = pool && n > 1 && rows >= RAY_PARALLEL_THRESHOLD;
        if (par) ray_pool_dispatch_n(pool, agg_key_scan_fn, &c, (uint32_t)n);
        else agg_key_scan_fn(&c, 0, 0, n);
        err = ray_scan_fail_finish(&fail, par ? pool : NULL);
        for (int64_t t = 0; !err && t < n; t++) {
            if (bounds[t * 2] < mn) mn = bounds[t * 2];
            if (bounds[t * 2 + 1] > mx) mx = bounds[t * 2 + 1];
        }
    }
    scratch_free(hdr);
    *lo = mn; *hi = mx;
    return err;
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
    const agg_rr_t* krr;     /* the keys; staged ones are read by pieces */
    const dense_plan_t* dp;
    const bool* candidate;
    uint32_t n_keys;
    int64_t rows;
    uint32_t tasks;
    uint64_t** task_bits;    /* [tasks][n_keys] private bitmaps, NULL when not a candidate */
    int64_t task;            /* the task its pieces belong to (agg_compact_mark_fn) */
} agg_compact_ctx_t;

static void agg_compact_mark_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t begin, int64_t limit) {
    (void)wid;
    agg_compact_ctx_t* c = raw;
    for (uint32_t k = 0; k < c->n_keys; k++) {
        if (!c->candidate[k]) continue;
        uint64_t* bits = c->task_bits[(size_t)c->task * c->n_keys + k];
        int64_t mn = c->dp->mins[k];
        bool nullable = c->dp->nullable[k];
        int64_t null = c->dp->nulls[k];
        const int8_t kt = c->krr->key_types[k];       /* hoisted: the bitmap stores */
        const uint8_t ka = v->key_attrs[k];           /* would reload them per row */
        const void* kd = v->key_data[k];
        const int64_t base = v->base;
        for (int64_t r = begin; r < limit; r++) {
            int64_t val = agg_read_key_i64_raw(kt, ka, kd, r - base);
            if (nullable && val == null) continue;
            uint64_t code = (uint64_t)(val - mn);
            bits[code >> 6] |= UINT64_C(1) << (code & 63);
        }
    }
}

static void agg_compact_mark_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    agg_compact_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->tasks * task;
        int64_t limit = task + 1 == c->tasks ? c->rows : c->rows / c->tasks * (task + 1);
        agg_compact_ctx_t tc = *c;   /* no atomics: a per-task copy names the task */
        tc.task = task;
        if (!c->krr->staged) {
            agg_view_t whole = agg_view_whole(c->krr);
            agg_compact_mark_rows(&tc, &whole, wid, begin, limit);
        } else (void)agg_walk(c->krr, &tc, agg_compact_mark_rows, wid, begin, limit, begin);
    }
}

static bool agg_dense_plan_compact(const agg_rr_t* krr, uint32_t n_keys, int64_t nrows,
                                   int64_t dense_limit, dense_plan_t* out, ray_t** err) {
    bool candidate[16];
    bool any = false;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t raw = out->ranges[k] - out->nullable[k];
        candidate[k] = raw > 1 && raw <= AGG_COMPACT_MAX_RANGE;
        any |= candidate[k];
    }
    if (!any) return false;
    /* The keys that are not candidates keep their ranges, and a candidate
     * compacts to at least one slot: when those ranges alone overflow the
     * limit no marking can make the plan fit (the check below would
     * fail), so the keys are not read. */
    {
        int64_t fixed = 1;
        for (uint32_t k = 0; k < n_keys; k++) {
            if (candidate[k]) continue;
            if (out->ranges[k] <= 0 || fixed > dense_limit / out->ranges[k]) return false;
            fixed *= out->ranges[k];
        }
    }
    /* Only the candidates are read: the others' staged sources leave the
     * walk (agg_compact_mark_rows never reads them). */
    ray_col_src_t* msrc[16];
    agg_rr_t mrr = *krr;
    if (krr->staged && krr->n_src <= 16) {
        for (uint32_t i = 0; i < krr->n_src; i++) msrc[i] = i < n_keys && !candidate[i] ? NULL : krr->srcs[i];
        mrr.srcs = msrc;
        krr = &mrr;
    }
    ray_pool_t* pool = ray_pool_get();
    uint32_t tasks = pool && nrows >= RAY_PARALLEL_THRESHOLD ? ray_pool_total_workers(pool) : 1;
    if ((int64_t)tasks > nrows) tasks = (uint32_t)nrows;
    size_t words_total = 0;
    for (uint32_t k = 0; k < n_keys; k++)
        if (candidate[k]) words_total += (size_t)((out->ranges[k] - out->nullable[k] + 63) / 64);
    /* Private bitmaps are words_total * tasks; keep the transient footprint
     * within AGG_COMPACT_BITMAP_BUDGET by using fewer, longer tasks. */
    while (tasks > 1 && words_total * tasks * sizeof(uint64_t) > AGG_COMPACT_BITMAP_BUDGET) tasks /= 2;
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
    agg_compact_ctx_t c = { .krr = krr, .dp = out, .candidate = candidate,
        .n_keys = n_keys, .rows = nrows, .tasks = tasks, .task_bits = task_bits };
    if (krr->staged) ray_scan_fail_init(krr->fail);
    if (tasks > 1) ray_pool_dispatch_n(pool, agg_compact_mark_fn, &c, tasks);
    else agg_compact_mark_fn(&c, 0, 0, 1);
    if (krr->staged && (*err = ray_scan_fail_finish(krr->fail, tasks > 1 ? pool : NULL)) != NULL) {
        ray_free_raw(bits); ray_free_raw(task_bits);
        return false;
    }
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
    memcpy(raw_ranges, out->ranges, (size_t)n_keys * sizeof(int64_t));
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
            memcpy(out->ranges, raw_ranges, (size_t)n_keys * sizeof(int64_t));
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

/* An estimate of the groups a key tuple forms — see agg_engine.h.
 *
 * The columns are sampled together at a fixed stride, AGG_EST_SAMPLE rows
 * over the input — the rows of `rows` when given, a selection's survivors,
 * since a filter that drops a dominant key value (the empty model of a
 * device column, on nine rows in ten) hides the groups and the shares of
 * what remains — (every row below that; the strided reads miss the cache,
 * and 32,768 of them over three columns cost half a millisecond at 10 M
 * rows with the prefetch below; the pages of a mapped column are asked
 * for ahead of them, ray_col_want_rows), one row from each window of `stride` rows
 * at an offset hashed from the window's index (agg_est_row), and each
 * column's values go through a small open-addressing set that counts how
 * often each was seen.
 * Blocks of adjacent rows would be cheaper to read but blind to clustered
 * keys: a run of four rows per value shows a block one value per four rows,
 * while a stride past the run length shows every sample distinct.  The
 * distinct count of the sample is then raised by the Chao1 species
 * estimator, d + f1 (f1 - 1) / 2 (f2 + 1) — f1 and f2 the values seen once
 * and twice — which answers for the values the sample missed: a key of 45
 * values has no singletons and stays 45; a skewed key, one value on nine
 * rows in ten and a million on the rest, shows thousands of singletons and
 * few pairs and lands near the million.  Each key's estimate is capped at
 * what it could hold, the span of its sampled values — its codes for a SYM
 * key, which are fixed once interned (the process-wide domain's count
 * bounds them too, but grows with every symbol interned afterwards, and
 * the estimate with it).
 *
 * The same pass hashes the tuple of the first n_prefix columns and the
 * tuple of all of them into two more sets, for the groups a composite key
 * forms (its columns' product overstates a city beside its country by the
 * number of countries) and for the tuples a (key, value) grouping would
 * keep.  A tuple holds at most the product of its columns' caps, and at
 * most one value per row.  A sorted prefix defeats the stride: every
 * sample is a different tuple once the run length is below the stride, and
 * the Chao1 saturates at the cap.  The sample notices — its prefix tuples
 * never decrease — and reads AGG_EST_BLOCKS blocks of AGG_EST_BLOCK adjacent
 * rows as well, which see the runs: the groups are the rows over the mean
 * run length, the tuples the distinct ones per block; the lower of the two
 * readings stands.  (A run longer than a block reads as a block, so blocks
 * alone would overstate a key of few groups; the stride is exact there.) */
enum { AGG_EST_SAMPLE = AGG_EST_SAMPLE_ROWS, AGG_EST_PREFETCH = 16,
       AGG_EST_BLOCK = 256, AGG_EST_BLOCKS = AGG_EST_SAMPLE / AGG_EST_BLOCK };

/* A sampled key value as int64: the integer itself (a null is its sentinel,
 * one more value), a SYM's code. */
static inline int64_t agg_est_read(const void* d, int8_t t, uint8_t attrs, int64_t r) {
    switch (t) {
    case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: return ((const int64_t*)d)[r];
    case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)d)[r];
    case RAY_I16: return ((const int16_t*)d)[r];
    case RAY_U8: case RAY_BOOL: return ((const uint8_t*)d)[r];
    default: return ray_read_sym(d, r, t, attrs);
    }
}

/* Fold one column's value into a tuple hash (order-sensitive). */
static inline uint64_t agg_est_mix(uint64_t h, int64_t v) {
    h ^= (uint64_t)v * 0x9E3779B97F4A7C15ULL;
    h *= 0xBF58476D1CE4E5B9ULL;
    return h ^ (h >> 31);
}

/* An open-addressing set of sampled values with how often each was seen,
 * kept as the Chao1 inputs d, f1, f2 and the count of the most frequent
 * (the share of the largest group).  A count is at most the sample. */
typedef struct {
    int64_t*  vals;
    uint16_t* cnt;
    int64_t   d, f1, f2, max;
} agg_est_set_t;

static inline void agg_est_set_add(agg_est_set_t* s, uint64_t mask, int64_t v) {
    uint64_t h = ((uint64_t)v * 0x9E3779B97F4A7C15ULL) >> 20 & mask;
    for (;; h = (h + 1) & mask) {
        if (s->cnt[h] == 0) {
            s->vals[h] = v; s->cnt[h] = 1; s->d++; s->f1++;
            if (s->max < 1) s->max = 1;
            return;
        }
        if (s->vals[h] == v) {
            uint16_t c = ++s->cnt[h];
            if (c == 2) { s->f1--; s->f2++; }
            else if (c == 3) s->f2--;
            if (c > s->max) s->max = c;
            return;
        }
    }
}

/* The Chao1 estimate from a set's counts (the distinct count itself when
 * the sample is the whole input), capped at `cap` when that is positive;
 * never below 1. */
static int64_t agg_est_chao1(const agg_est_set_t* s, bool sampled, int64_t cap) {
    int64_t est = s->d;
    if (sampled && s->f1 > 1) {
        /* f1 <= AGG_EST_SAMPLE: the product is far from overflowing */
        int64_t add = s->f1 * (s->f1 - 1) / (2 * (s->f2 + 1));
        est = add > INT64_MAX - est ? INT64_MAX : est + add;
    }
    if (cap > 0 && cap < est) est = cap;
    return est < 1 ? 1 : est;
}

/* What a tuple of columns 0..n can hold: the product of their caps (a
 * column without one, or a product past nrows, is nrows) and nrows. */
static int64_t agg_est_tuple_cap(const int64_t* cap, uint32_t n, int64_t nrows) {
    int64_t prod = 1;
    for (uint32_t k = 0; k < n; k++) {
        if (cap[k] <= 0 || prod > nrows / cap[k]) return nrows;
        prod *= cap[k];
    }
    return prod < nrows ? prod : nrows;
}

/* Blocks of adjacent rows over a sorted prefix: the groups as rows over the
 * mean run length of the prefix tuple, the (prefix, rest) tuples as the
 * distinct ones per block scaled to the input.  false when the input is too
 * short for the blocks (the stride is then exact anyway). */
static bool agg_est_blocks(ray_t** cols, uint32_t n, uint32_t n_prefix,
                           const int64_t* rows, int64_t nrows, int64_t* pos,
                           int64_t* prefix_est, int64_t* all_est) {
    enum { SLOTS = AGG_EST_BLOCK * 4 };
    if (nrows < (int64_t)AGG_EST_BLOCK * 4) return false;
    int64_t nblk = AGG_EST_BLOCKS;
    if (nblk * AGG_EST_BLOCK > nrows) nblk = nrows / AGG_EST_BLOCK;
    const int64_t gap = nrows / nblk;
    /* the blocks' rows listed first (`pos` holds AGG_EST_SAMPLE), so the
     * pages of a mapped column are asked for ahead of the reads, as the
     * strided sample's are */
    int64_t np = 0;
    for (int64_t b = 0; b < nblk; b++)
        for (int64_t i = b * gap; i < b * gap + AGG_EST_BLOCK; i++) pos[np++] = rows ? rows[i] : i;
    for (uint32_t k = 0; k < n; k++) ray_col_want_rows(cols[k], pos, np);
    int64_t  vals[SLOTS];
    uint16_t cnt[SLOTS];
    agg_est_set_t set = { vals, cnt, 0, 0, 0, 0 };
    int64_t runs = 0, read = 0, distinct = 0;
    np = 0;
    for (int64_t b = 0; b < nblk; b++) {
        memset(cnt, 0, sizeof cnt);
        set.d = set.f1 = set.f2 = set.max = 0;
        uint64_t hp_prev = 0;
        for (int64_t i = 0; i < AGG_EST_BLOCK; i++) {
            const int64_t r = pos[np++];
            uint64_t hp = 0, ha = 0;
            for (uint32_t k = 0; k < n; k++) {
                ha = agg_est_mix(ha, agg_est_read(ray_data(cols[k]), cols[k]->type,
                                                  cols[k]->attrs, r));
                if (k + 1 == n_prefix) hp = ha;
            }
            if (i == 0 || hp != hp_prev) runs++;
            hp_prev = hp;
            agg_est_set_add(&set, SLOTS - 1, (int64_t)ha);
        }
        distinct += set.d;
        read += AGG_EST_BLOCK;
    }
    *prefix_est = (int64_t)((double)nrows * (double)runs / (double)read);
    *all_est    = (int64_t)((double)nrows * (double)distinct / (double)read);
    if (*prefix_est < 1) *prefix_est = 1;
    if (*all_est < 1) *all_est = 1;
    return true;
}

bool agg_group_card_sample(ray_t** cols, uint32_t n, uint32_t n_prefix,
                           const int64_t* rows, int64_t nrows, int64_t population,
                           int64_t* col_est, int64_t* prefix_est, int64_t* prefix_max,
                           int64_t* all_est) {
    if (n < 1 || n > 16 || n_prefix < 1 || n_prefix > n || nrows <= 0) return false;
    /* the rows the estimates describe: the list itself, or the population
     * it was sampled from */
    const int64_t pop = population > nrows ? population : nrows;
    int64_t     cap[16];
    const void* data[16];
    size_t      esz[16];
    for (uint32_t k = 0; k < n; k++) {
        ray_t* c = cols[k];
        switch (c->type) {
        case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
        case RAY_BOOL: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP:
        case RAY_SYM:
            break;
        default:
            return false;
        }
        data[k] = ray_data(c);
        esz[k]  = col_esz(c);
    }
    const bool    tuples = prefix_est || prefix_max || all_est;
    const int64_t sample = nrows < AGG_EST_SAMPLE ? nrows : AGG_EST_SAMPLE;
    uint64_t slots = 64;
    while (slots < (uint64_t)sample * 2) slots <<= 1;
    const uint32_t nsets = n + (tuples ? 2 : 0);
    const size_t   set_bytes = (size_t)slots * (sizeof(int64_t) + sizeof(uint16_t));
    uint8_t* mem = ray_alloc_raw(set_bytes * nsets + (size_t)AGG_EST_SAMPLE * sizeof(int64_t));
    if (!mem) return false;
    /* The rows the pass reads, listed first: the pages of a mapped column
     * are asked for ahead of the reads, through the pool, as the select's
     * own scan asks for its.  Read as they were met, each strided row of a
     * column not in memory waited on its own page fault, on this thread —
     * a quarter second and more per column at 100 M rows, where the pass
     * costs a millisecond in memory.  The pages asked for are those the
     * sampled rows fall on (ray_col_want_rows), or the rows between the
     * first and last sampled as one range when the rows the estimate
     * describes fall on every page of that span — a row per page or more:
     * the select reads the span whole then, and its stream runs while this
     * pass waits on the first of them; a sample's pages alone, 30,000
     * reads of one page each, took twice as long as the stream.  The span,
     * not the column: a selection over a slice of a time-ordered table
     * asks for its slice (a 1% window asked for the columns whole before,
     * 90 MB read for 2 needed at 4 M rows). */
    int64_t* pos = (int64_t*)(mem + set_bytes * nsets);
    for (int64_t i = 0; i < sample; i++) {
        const int64_t ri = agg_est_sample_row(i, sample, nrows);
        pos[i] = rows ? rows[ri] : ri;
    }
    int64_t lo_r = pos[0], hi_r = pos[0];
    for (int64_t i = 1; i < sample; i++) {
        if (pos[i] < lo_r) lo_r = pos[i];
        if (pos[i] > hi_r) hi_r = pos[i];
    }
    for (uint32_t k = 0; k < n; k++) {
        const size_t span = (size_t)(hi_r - lo_r + 1) * esz[k];
        if (pop >= (int64_t)(span >> 12)) {
            if (cols[k]->mmod == 1 && ray_pool_scan_on()) {
                const uint8_t* p = (const uint8_t*)data[k] + (size_t)lo_r * esz[k];
                if (!ray_vm_resident(p, span)) ray_pool_want(p, span);
            }
        } else {
            ray_col_want_rows(cols[k], pos, sample);
        }
    }
    agg_est_set_t sets[18];
    for (uint32_t i = 0; i < nsets; i++) {
        sets[i].vals = (int64_t*)(mem + (size_t)i * set_bytes);
        sets[i].cnt  = (uint16_t*)(mem + (size_t)i * set_bytes + (size_t)slots * sizeof(int64_t));
        memset(sets[i].cnt, 0, (size_t)slots * sizeof(uint16_t));
        sets[i].d = sets[i].f1 = sets[i].f2 = sets[i].max = 0;
    }
    const uint64_t mask = slots - 1;
    int64_t lo[16], hi[16], prev[16];
    for (uint32_t k = 0; k < n; k++) { lo[k] = INT64_MAX; hi[k] = INT64_MIN; prev[k] = INT64_MIN; }
    bool sorted = true;   /* the sampled prefix tuples never decrease */
    for (int64_t i = 0; i < sample; i++) {
        const int64_t r = pos[i];
        if (i + AGG_EST_PREFETCH < sample) {
            /* the strided reads miss the cache; keep several in flight */
            const size_t rp = (size_t)pos[i + AGG_EST_PREFETCH];
            for (uint32_t k = 0; k < n; k++)
                __builtin_prefetch((const char*)data[k] + rp * esz[k]);
        }
        uint64_t hp = 0, ha = 0;
        int cmp = 0;   /* the prefix tuple against the previous sample's */
        for (uint32_t k = 0; k < n; k++) {
            const int64_t v = agg_est_read(data[k], cols[k]->type, cols[k]->attrs, r);
            if (v < lo[k]) lo[k] = v;
            if (v > hi[k]) hi[k] = v;
            agg_est_set_add(&sets[k], mask, v);
            if (tuples) {
                ha = agg_est_mix(ha, v);
                if (k < n_prefix) {
                    hp = ha;
                    if (cmp == 0 && v != prev[k]) cmp = v < prev[k] ? -1 : 1;
                    prev[k] = v;
                }
            }
        }
        if (tuples) {
            if (cmp < 0) sorted = false;
            agg_est_set_add(&sets[n], mask, (int64_t)hp);
            agg_est_set_add(&sets[n + 1], mask, (int64_t)ha);
        }
    }
    const bool sampled = sample < pop;
    for (uint32_t k = 0; k < n; k++) {
        /* the span of the sampled values (codes) bounds what the key can hold */
        uint64_t span = (uint64_t)hi[k] - (uint64_t)lo[k];
        cap[k] = span < (uint64_t)INT64_MAX - 1 ? (int64_t)span + 1 : 0;
        int64_t est = agg_est_chao1(&sets[k], sampled, cap[k]);
        if (col_est) col_est[k] = est;
    }
    if (tuples) {
        int64_t kp = agg_est_chao1(&sets[n], sampled, agg_est_tuple_cap(cap, n_prefix, pop));
        int64_t ka = agg_est_chao1(&sets[n + 1], sampled, agg_est_tuple_cap(cap, n, pop));
        int64_t bp, ba;
        /* adjacent entries of a sampled list are not adjacent rows */
        if (sorted && sampled && pop == nrows &&
            agg_est_blocks(cols, n, n_prefix, rows, nrows, pos, &bp, &ba)) {
            if (bp < kp) kp = bp;
            if (ba < ka) ka = ba;
        }
        if (prefix_est) *prefix_est = kp;
        if (all_est) *all_est = ka;
        /* the largest group's rows: its share of the sample over the population */
        if (prefix_max) *prefix_max = sets[n].max * (pop / sample) +
                                      sets[n].max * (pop % sample) / sample;
    }
    ray_free_raw(mem);
    return true;
}

int64_t agg_group_card_estimate(ray_t** key_cols, uint32_t n_keys, int64_t nrows) {
    int64_t est[16];
    if (n_keys < 1 || n_keys > 16 ||
        !agg_group_card_sample(key_cols, n_keys, n_keys, NULL, nrows, 0, est, NULL, NULL, NULL))
        return -1;
    int64_t total = 1;
    for (uint32_t k = 0; k < n_keys; k++) {
        if (est[k] < 1) return -1;   /* never below 1 (static analysis) */
        if (total > INT64_MAX / est[k]) return -1;
        total *= est[k];
    }
    return total;
}

/* The bytes the calling heap can take before the anonymous watermark: the
 * room under it, with the committed blocks the heap can reuse added back.
 * 0 when the watermark (the RAM) is unknown. */
static int64_t agg_dense_wide_room(void) {
    int64_t wm = ray_heap_anon_watermark();
    if (wm <= 0) return 0;
    int64_t room = wm - ray_heap_anon_committed() + ray_heap_anon_reusable();
    return room > 0 ? room : 0;
}

/* Whether a shared slab of this plan holds u32 counters (4 bytes a slot): every
 * aggregate is COUNT, so one counter per slot serves them all, and the input
 * has at most UINT32_MAX rows, so no slot's count can pass UINT32_MAX.  With
 * more rows the slab keeps the 8-byte count states. */
static bool agg_count32_slab(const agg_vtable_t** vts, uint32_t n_aggs, int64_t rows) {
    if (n_aggs == 0 || rows > (int64_t)UINT32_MAX) return false;
    const agg_vtable_t* cnt = agg_resolve(OP_COUNT, RAY_I64);
    for (uint32_t a = 0; a < n_aggs; a++) if (vts[a] != cnt) return false;
    return true;
}

/* A wide plan spans at most this many slots per input row: a slab much larger
 * than the input is mostly empty, and the radix routes answer from the rows. */
#define AGG_WIDE_RANGE_PER_ROW 16

/* The most slots a wide dense plan (a range past the row count) may span: one
 * four-byte counter per slot must fit the room, and a slot is a uint32. */
static int64_t agg_dense_wide_slots(void) {
    int64_t s = agg_dense_wide_room() / 4;
    return s < (int64_t)UINT32_MAX ? s : (int64_t)UINT32_MAX;
}

/* Whether the single key has many groups: of 1024 rows spread over the table,
 * at least a quarter hold distinct keys.  A wide dense plan pays for its
 * whole range, so a key with a few groups spread over a wide range (which
 * the table stack and the hash routes answer from their own few entries)
 * does not take it.  *err is set on a failed range read. */
static bool agg_wide_populated(ray_t* key_col, const agg_rr_t* rr, int64_t nrows, ray_t** err) {
    enum { SAMPLES = 1024, CAP = 2048 };
    *err = NULL;
    if (nrows < SAMPLES) return false;
    int64_t seen[CAP];
    bool used[CAP];
    memset(used, 0, sizeof used);
    agg_curs_t cs; cs.cur = NULL;
    if (rr->staged && !agg_curs_init(&cs, rr)) { *err = ray_error("oom", NULL); return false; }
    int64_t step = nrows / SAMPLES, distinct = 0;
    if (!rr->staged && ray_col_cold(key_col))
        for (int i = 0; i < SAMPLES; i++)
            ray_col_want_row(key_col, i * step + (int64_t)(ray_hash_i64(i) % (uint64_t)step));
    for (int i = 0; i < SAMPLES && !*err; i++) {
        int64_t v = agg_key_at(rr, &cs, 0, i * step + (int64_t)(ray_hash_i64(i) % (uint64_t)step), err);
        if (*err) break;
        uint32_t at = ray_hash_i64(v) & (CAP - 1);
        while (used[at] && seen[at] != v) at = (at + 1) & (CAP - 1);
        if (!used[at]) { used[at] = true; seen[at] = v; distinct++; }
    }
    agg_curs_free(&cs, rr);
    return !*err && distinct * 4 >= SAMPLES;
}

/* Whether a single-key plan of total_slots runs as the one shared slab of a
 * wide plan: every aggregate updates a shared state, the slab and its
 * occupancy bits fit three quarters of the room under the anonymous
 * watermark, and the key shows many groups (agg_wide_populated).  The
 * caller checks the plan's shape (pool, rows, streaming states, no group
 * limit, one key).  *err is set on a failed range read. */
static bool agg_wide_shared_fits(const agg_vtable_t** vts, uint32_t n_aggs, size_t block,
                                 int64_t total_slots, int64_t eff_n, ray_t* key_col,
                                 const agg_rr_t* rr, int64_t nrows, ray_t** err) {
    *err = NULL;
    if (agg_no_wide_dense) return false;
    for (uint32_t a = 0; a < n_aggs; a++)
        if (vts[a]->update_shared == NULL) return false;
    size_t slot_bytes = agg_count32_slab(vts, n_aggs, eff_n) ? sizeof(uint32_t) : block;
    double need = (double)total_slots * (double)slot_bytes + (double)total_slots / 8;
    if (need > (double)agg_dense_wide_room() / 4 * 3) return false;
    return agg_wide_populated(key_col, rr, nrows, err);
}

/* agg_dense_plan over keys bound for range reads: krr's staged keys (their
 * key_cols are stand-ins) take their bounds from the directory or by
 * pieces.  False with *err set: a range error. */
static bool agg_dense_plan_src(ray_t** key_cols, const agg_rr_t* krr, uint32_t n_keys, bool allow_wide,
                               int64_t nrows, dense_plan_t* out, ray_t** err) {
    *err = NULL;
    out->ok = false;
    out->n_keys = n_keys;
    out->compacted = false;
    out->wide = false;
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
        if (krr && krr->srcs[k]) {
            if ((*err = agg_key_bounds_staged(krr, k, nrows, out->nullable[k], out->nulls[k], &mn, &mx)) != NULL)
                return false;
        } else agg_key_bounds_parallel(kc, nrows, out->nullable[k], out->nulls[k], &mn, &mx);
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
    /* One key whose range passes the row count: a slab the size of the range
     * is admitted when it is at most AGG_WIDE_RANGE_PER_ROW slots per row and
     * fits the room under the memory watermark (agg_dense_wide_slots).  Its
     * answer is the same; only the shared strategy runs it (the caller drops
     * the plan otherwise). */
    if (overflow && n_keys == 1 && allow_wide && out->ranges[0] <= AGG_WIDE_RANGE_PER_ROW * nrows &&
            out->ranges[0] <= agg_dense_wide_slots()) {
        overflow = false;
        out->wide = true;
        total = out->ranges[0];
    }
    if (overflow || (n_keys >= 2 && total > AGG_COMPACT_TRY_SLOTS)) {
        /* Raw ranges multiply out too far — or far enough past a
         * cache-resident slab that sparse keys would waste it (a W16 SYM key
         * reports its whole 65,536-code width): compact the keys to the codes
         * they use and retry the packing over the compacted ranges.  Keys
         * whose every code is used keep their raw range. */
        bool compacted_ok = n_keys >= 2 &&
            agg_dense_plan_compact(krr, n_keys, nrows, dense_limit, out, err);
        if (*err) return false;
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

/* The keys of plain columns, as a keys-only source set (nothing staged). */
static agg_rr_t agg_keys_plain_rr(ray_t** key_cols, uint32_t n_keys, ray_t** hdr,
                                  ray_scan_fail_t* fail) {
    agg_rr_t rr = { .n_src = n_keys, .n_keys = n_keys, .fail = fail };
    size_t nk = n_keys ? n_keys : 1;
    void* blk = scratch_calloc(hdr, nk * (2 * sizeof(void*) + 3));
    if (!blk) { rr.n_src = rr.n_keys = 0; return rr; }
    ray_col_src_t** srcs = (ray_col_src_t**)blk;
    const void** plain = (const void**)(srcs + nk);
    uint8_t* esz = (uint8_t*)(plain + nk);
    int8_t* types = (int8_t*)(esz + nk);
    uint8_t* attrs = (uint8_t*)(types + nk);
    for (uint32_t k = 0; k < n_keys; k++) {
        plain[k] = ray_data(key_cols[k]);
        types[k] = key_cols[k]->type;
        attrs[k] = key_cols[k]->attrs;
        esz[k] = ray_sym_elem_size(key_cols[k]->type, key_cols[k]->attrs);
    }
    rr.srcs = srcs; rr.plain = plain; rr.esz = esz; rr.key_types = types; rr.key_attrs = attrs;
    return rr;
}

bool agg_dense_plan(ray_t** key_cols, uint32_t n_keys,
                    const agg_vtable_t** vts, uint32_t n_aggs,
                    int64_t nrows, dense_plan_t* out) {
    (void)vts; (void)n_aggs;   /* agg kind no longer gates dense eligibility */
    out->ok = false;           /* a failed carve leaves a plan agg_dense_plan_free takes */
    out->compacted = false;
    memset(out->remap, 0, sizeof(out->remap));
    memset(out->inverse, 0, sizeof(out->inverse));
    ray_t* hdr = NULL;
    ray_scan_fail_t fail;
    ray_scan_fail_init(&fail);
    agg_rr_t krr = agg_keys_plain_rr(key_cols, n_keys, &hdr, &fail);
    ray_t* err = NULL;
    bool ok = krr.n_keys == n_keys && agg_dense_plan_src(key_cols, &krr, n_keys, false, nrows, out, &err);
    if (err) ray_error_free(err);   /* plain keys read in place: no range errors */
    scratch_free(hdr);
    return ok;
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

typedef struct { const ray_t* v; _Atomic(int) found; } key_null_scan_t;

/* Any null sentinel in [start, end)?  Written as a block-wise OR-reduction
 * so the compiler vectorises the compare; the block boundary is where the
 * early exit happens. */
static bool key_null_in_range(const ray_t* v, int64_t start, int64_t end) {
    const void* p = ray_data((ray_t*)v);
    enum { BLOCK = 4096 };
    for (int64_t s = start; s < end; s += BLOCK) {
        int64_t e = s + BLOCK < end ? s + BLOCK : end;
        int any = 0;
        switch (v->type) {
            case RAY_F64: { const double* d = p; for (int64_t i = s; i < e; i++) any |= (d[i] != d[i]); break; }
            case RAY_F32: { const float* d = p; for (int64_t i = s; i < e; i++) any |= (d[i] != d[i]); break; }
            case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: { const int64_t* d = p; for (int64_t i = s; i < e; i++) any |= (d[i] == NULL_I64); break; }
            case RAY_I32: case RAY_DATE: case RAY_TIME: { const int32_t* d = p; for (int64_t i = s; i < e; i++) any |= (d[i] == NULL_I32); break; }
            case RAY_I16: { const int16_t* d = p; for (int64_t i = s; i < e; i++) any |= (d[i] == NULL_I16); break; }
            case RAY_GUID: { const uint64_t* d = p; for (int64_t i = s; i < e; i++) any |= (d[2 * i] == 0 && d[2 * i + 1] == 0); break; }
            default: return false;
        }
        if (any) return true;
    }
    return false;
}

static void key_null_scan_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    key_null_scan_t* c = raw;
    if (atomic_load_explicit(&c->found, memory_order_relaxed)) return;
    if (key_null_in_range(c->v, start, end)) atomic_store_explicit(&c->found, 1, memory_order_relaxed);
}

/* A result key column inherits its source column's "may have nulls" flag,
 * but a fused `where:` can remove every null row, and the same query over
 * the filtered table materialized first carries no flag: one result, two
 * spellings on the wire.  For a typed key keep the flag only when a key
 * value is null: a vectorised sentinel scan, split across the pool for a
 * large column.  Text keys keep theirs: for them the flag is always "may". */
void ray_group_key_settle_nulls(ray_t* kc) {
    if (!kc || RAY_IS_ERR(kc) || !(kc->attrs & RAY_ATTR_HAS_NULLS)) return;
    switch (kc->type) {
        case RAY_F64: case RAY_F32: case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP:
        case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_I16: case RAY_GUID: break;
        default: return;
    }
    key_null_scan_t c = { kc, 0 };
    ray_pool_t* pool = ray_pool_get();
    if (ray_pool_par_dispatch_ok(pool, kc->len, RAY_PARALLEL_THRESHOLD))
        ray_pool_dispatch(pool, key_null_scan_fn, &c, kc->len);
    else key_null_scan_fn(&c, 0, 0, kc->len);
    if (!atomic_load_explicit(&c.found, memory_order_relaxed))
        kc->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
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
            /* A failed append returns an error and leaves `out`, with the
             * items it already retained, to the caller. */
            ray_t* next = ray_list_append(out, value);
            if (!next || RAY_IS_ERR(next)) {
                ray_release(out);
                return next ? next : ray_error("oom", NULL);
            }
            out = next;
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
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: GATHER_NULL(int64_t, NULL_I64); break;
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
    bool gathered_null = atomic_load_explicit(&c.any_null, memory_order_relaxed) &&
                         src_col->type != RAY_BOOL && src_col->type != RAY_U8;
    if (gathered_null || ray_vec_may_have_nulls(src_col)) out->attrs |= RAY_ATTR_HAS_NULLS;
    if (!gathered_null) ray_group_key_settle_nulls(out);
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
    int64_t         row_end;    /* past the segment of the range's last selected row */
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
    c->row_end = c->nrows;
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
    /* And the one holding the last: the rows read ahead end there. */
    for (hi = c->n_segs; lo < hi; ) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (prefix[mid + 1] <= sel_end - 1) lo = mid + 1;
        else hi = mid;
    }
    if ((int64_t)(lo + 1) * RAY_MORSEL_ELEMS < c->nrows) c->row_end = (int64_t)(lo + 1) * RAY_MORSEL_ELEMS;
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
    /* A pass over the selection reads these rows next, in order: often the
     * first read of its aggregate inputs (scan read-ahead; the filter's
     * zones keep the chunks it rules out from being requested). */
    if (n) ray_pool_scan_at(c->nrows, rows[0], c->row_end);
    return n;
}

/* Gather one chunk's value column (native esz) into a small dense buffer:
 * rows[i] read at rows[i] - base (src is the view's pointer at row base).
 * COUNT (val_data == NULL) needs no gather — returns without touching dst. */
static inline void agg_sel_gather_vals(void* dst, const void* src, uint8_t esz,
                                       const int64_t* rows, int64_t n, int64_t base) {
    if (!src || esz == 0) return;
    const char* s = (const char*)src;
    switch (esz) {
        case 8: {
            int64_t* dd = (int64_t*)dst; const int64_t* ss = (const int64_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i] - base];
            break;
        }
        case 4: {
            int32_t* dd = (int32_t*)dst; const int32_t* ss = (const int32_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i] - base];
            break;
        }
        case 2: {
            int16_t* dd = (int16_t*)dst; const int16_t* ss = (const int16_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i] - base];
            break;
        }
        case 1: {
            uint8_t* dd = (uint8_t*)dst; const uint8_t* ss = (const uint8_t*)s;
            for (int64_t i = 0; i < n; i++) dd[i] = ss[rows[i] - base];
            break;
        }
        default: {  /* general gather, correct for any element width */
            char* dd = (char*)dst;
            for (int64_t i = 0; i < n; i++) memcpy(dd + i * esz, s + (rows[i] - base) * esz, esz);
            break;
        }
    }
}

/* Per-dispatch descriptor tables, sized from ext->n_keys / ext->n_aggs.
 * One carve; 8-byte arrays first, then narrower.  Replaces the fixed
 * [16] blocks that used to cap the engine at 16 keys/aggs.
 *
 * key_data, val_data and val2_data are one array (the sources' row-0
 * pointers, NULL for a staged source and for COUNT), key_srcs, val_srcs
 * and val2_srcs another (the staged sources), key_esz, val_esz and
 * val2_esz a third: rr reads them as flat [n_keys + 2 * n_aggs] arrays.
 * val_id / val2_id name each input whether staged or plain (its row-0
 * pointer, or its source; NULL: no input): the layouts and the phases that
 * read no column test presence and identity on them. */
typedef struct {
    ray_t*          hdr;
    const void**    key_data;
    const void**    val_data;  const void** val2_data;
    ray_col_src_t** key_srcs;
    ray_col_src_t** val_srcs;  ray_col_src_t** val2_srcs;
    const void**    val_id;    const void** val2_id;
    int64_t*        agg_syms;
    uint8_t*        key_esz;
    int8_t*         key_types; uint8_t* key_attrs;
    int8_t*         val_types; int8_t*  val2_types;
    bool*           val_hasnull; bool*  val2_hasnull;
    uint8_t*        val_esz;   uint8_t* val2_esz;
    bool            staged;    /* some key or input is read by range */
    agg_rr_t        rr;
    agg_view_t      whole;
    ray_scan_fail_t fail;      /* the strategy's dispatches (R-FAIL) */
} agg_desc_t;

/* A value column: bound, decisions from the source; the payload pointer is
 * captured only when the column can be read in place (ray_col_src_ready:
 * generation 0, or a published materialization).  Otherwise *plain stays
 * NULL and the strategies read the source by range.  Absent leaves *src
 * NULL; a failed bind sets *err.  The agg shapes admitted before this
 * point never name a partition key or a sliced parted column, so a source
 * that does not bind is an error here, not a fall back to raw reads. */
static bool agg_desc_value(ray_graph_t* g, ray_t* tbl, int64_t sym, const void** plain,
                           ray_col_src_t** src, ray_t** err) {
    *plain = NULL; *src = NULL;
    if (ray_col_bind_declines(tbl, sym)) {
        *err = ray_error("nyi", "aggregate input is not a bindable column");
        return false;
    }
    *src = ray_graph_bind_col(g, tbl, sym, err);
    if (!*src) return *err == NULL;
    ray_t* ready = ray_col_src_ready(*src);   /* ready-only: never decodes */
    if (ready && !RAY_IS_PARTED(ready->type)) *plain = ray_data(ready);
    return true;
}

/* False with *err NULL: out of memory; false with an owned *err: a column
 * bind failed (the caller returns it).  A key whose column is a stand-in
 * (its source is bound and not ready as that column: exec_group_v2_run_inner)
 * and an input that is not ready are staged: read by range. */
static bool agg_desc_init(agg_desc_t* d, ray_graph_t* g, ray_op_ext_t* ext,
                          ray_t* tbl, ray_t** key_cols, ray_t** err) {
    *err = NULL;
    uint32_t nk = ext->n_keys, na = ext->n_aggs;
    uint32_t ns = nk + 2u * na;
    size_t n8 = 2u * (size_t)ns + 3u * na;         /* plain, srcs, ids, agg_syms */
    size_t n1 = 3u * (size_t)nk + 6u * na;         /* esz, key types/attrs, val types/hasnull */
    d->key_data = (const void**)scratch_calloc(&d->hdr, n8 * 8 + n1 + 1);
    if (!d->key_data) return false;
    d->val_data   = d->key_data + nk;
    d->val2_data  = d->val_data + na;
    d->key_srcs   = (ray_col_src_t**)(d->val2_data + na);
    d->val_srcs   = d->key_srcs + nk;
    d->val2_srcs  = d->val_srcs + na;
    d->val_id     = (const void**)(d->val2_srcs + na);
    d->val2_id    = d->val_id + na;
    d->agg_syms   = (int64_t*)(d->val2_id + na);
    d->key_esz    = (uint8_t*)(d->agg_syms + na);
    d->val_esz    = d->key_esz + nk;
    d->val2_esz   = d->val_esz + na;
    d->key_types  = (int8_t*)(d->val2_esz + na);
    d->key_attrs  = (uint8_t*)(d->key_types + nk);
    d->val_types  = (int8_t*)(d->key_attrs + nk);
    d->val2_types = d->val_types + na;
    d->val_hasnull  = (bool*)(d->val2_types + na);
    d->val2_hasnull = d->val_hasnull + na;
    d->staged = false;
    for (uint32_t k = 0; k < nk; k++) {
        ray_t* kc = key_cols[k];
        d->key_types[k] = kc->type;
        d->key_attrs[k] = kc->attrs;
        d->key_esz[k] = ray_sym_elem_size(kc->type, kc->attrs);
        d->key_data[k] = ray_data(kc);
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]);
        if (!kext || ray_col_bind_declines(tbl, kext->sym)) continue;   /* today's column */
        ray_col_src_t* ks = ray_graph_bind_col(g, tbl, kext->sym, err);
        if (*err) goto fail;
        if (ks && ray_col_src_ready(ks) != kc) {   /* kc is the stand-in */
            d->key_srcs[k] = ks;
            d->key_data[k] = NULL;
            d->staged = true;
        }
    }
    for (uint32_t a = 0; a < na; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        d->agg_syms[a] = ie ? ie->sym : 0;
        const void* vp = NULL;
        ray_col_src_t* vs = NULL;
        if (ext->agg_ops[a] != OP_COUNT && ie &&
            !agg_desc_value(g, tbl, ie->sym, &vp, &vs, err)) goto fail;
        d->val_data[a]    = vp;
        d->val_srcs[a]    = vs && !vp ? vs : NULL;
        d->val_types[a]   = vs ? vs->type : RAY_I64;
        d->val_hasnull[a] = vs ? vs->may_null : false;
        d->val_esz[a]     = vs ? ray_col_src_esz(vs) : 0;
        d->val_id[a]      = vp ? vp : (const void*)vs;
        d->staged |= d->val_srcs[a] != NULL;
        /* Binary agg (pearson): resolve the y-side column from agg_ins2[a]. */
        const void* vp2 = NULL;
        ray_col_src_t* vs2 = NULL;
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE &&
            !agg_desc_value(g, tbl, find_ext(g, ext->agg_ins2[a])->sym, &vp2, &vs2, err)) goto fail;
        d->val2_data[a]    = vp2;
        d->val2_srcs[a]    = vs2 && !vp2 ? vs2 : NULL;
        d->val2_types[a]   = vs2 ? vs2->type : RAY_I64;
        d->val2_hasnull[a] = vs2 ? vs2->may_null : false;
        d->val2_esz[a]     = vs2 ? ray_col_src_esz(vs2) : 0;
        d->val2_id[a]      = vp2 ? vp2 : (const void*)vs2;
        d->staged |= d->val2_srcs[a] != NULL;
    }
    ray_scan_fail_init(&d->fail);
    d->rr = (agg_rr_t){ .srcs = d->key_srcs, .plain = d->key_data, .esz = d->key_esz,
        .key_types = d->key_types, .key_attrs = d->key_attrs,
        .n_src = ns, .n_keys = nk, .n_aggs = na, .fail = &d->fail, .staged = d->staged };
    d->whole = agg_view_whole(&d->rr);
    return true;
fail:
    scratch_free(d->hdr); d->hdr = NULL;
    return false;
}

/* One read-ahead hint per staged source over the rows a strategy reads. */
static void agg_desc_advise(const agg_desc_t* d, int64_t nrows) {
    for (uint32_t i = 0; d->staged && i < d->rr.n_src; i++)
        if (d->rr.srcs[i]) ray_col_src_advise(d->rr.srcs[i], 0, nrows);
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
        ray_col_meta_t vm;
        bool vh = ext->agg_ops[a] != OP_COUNT && ie && agg_col_meta(g, tbl, ie->sym, &vm);
        int8_t in_type = vh ? vm.type : RAY_I64;
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
 * (i.e. states_base + off applied per agg by the kernel via off[a]).  The
 * values come from view v (rows read at rows[i] - v->base: the whole columns,
 * or a run of rows inside the staged sources' views).  Returns 0, or -1 on
 * scratch OOM (sets sc->oom).  shared: the kernels' update_shared (atomic) in
 * place of update_batch, for a slab several tasks update at once. */
static int agg_sel_accum_chunk(const agg_valdesc_t* vd, const agg_view_t* v, agg_sel_scratch_t* sc,
                               char* states, const uint32_t* gid,
                               const int64_t* rows, int64_t n, bool shared) {
    for (uint32_t a = 0; a < vd->n_aggs; a++) {
        const agg_vtable_t* vt = vd->vts[a];
        if (vt->update_batch2) {                            /* binary agg (pearson) */
            uint8_t ez = vd->val_esz[a], ez2 = vd->val2_esz[a];
            if (!sc->gv[a]) { sc->gv[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * (ez ? ez : 1)); if (!sc->gv[a]) { sc->oom = 1; return -1; } }
            if (!sc->gy[a]) { sc->gy[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * (ez2 ? ez2 : 1)); if (!sc->gy[a]) { sc->oom = 1; return -1; } }
            agg_sel_gather_vals(sc->gv[a], v->val_data[a],  ez,  rows, n, v->base);
            agg_sel_gather_vals(sc->gy[a], v->val2_data[a], ez2, rows, n, v->base);
            ray_valid_t vx = { sc->gv[a], vd->val_types[a],  vd->val_hasnull[a] };
            ray_valid_t vy = { sc->gy[a], vd->val2_types[a], vd->val2_hasnull[a] };
            vt->update_batch2(states + vd->off[a], vd->block, gid,
                              sc->gv[a], sc->gy[a], &vx, &vy, n, NULL);
        } else if (v->val_data[a]) {                        /* unary agg over a column */
            uint8_t ez = vd->val_esz[a];
            if (!sc->gv[a]) { sc->gv[a] = ray_alloc_raw((size_t)AGG_SEL_CHUNK * (ez ? ez : 1)); if (!sc->gv[a]) { sc->oom = 1; return -1; } }
            agg_sel_gather_vals(sc->gv[a], v->val_data[a], ez, rows, n, v->base);
            ray_valid_t valid = { sc->gv[a], vd->val_types[a], vd->val_hasnull[a] };
            if (shared) vt->update_shared(states + vd->off[a], vd->block, gid, sc->gv[a], &valid, n);
            else vt->update_batch(states + vd->off[a], vd->block, gid, sc->gv[a], &valid, n, NULL);
        } else {                                            /* COUNT (no value gather) */
            ray_valid_t valid = { NULL, vd->val_types[a], false };
            if (shared) vt->update_shared(states + vd->off[a], vd->block, gid, NULL, &valid, n);
            else vt->update_batch(states + vd->off[a], vd->block, gid, NULL, &valid, n, NULL);
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
    const agg_rr_t* krr;     /* the keys: plain ones read in place, staged by R-SPARSE */
    const bool* nullable;
    const int64_t* nulls;
    uint32_t n_keys;
    ray_t* sel;
    const int64_t* sel_prefix;
    int64_t n_sel;
    uint32_t tasks;
    const int64_t* spans;   /* [tasks][2] selected-ordinal ranges; NULL: n_sel split evenly */
    int64_t* task_bounds;   /* [tasks][2][n_keys]: min row, max row per key */
} agg_sel_bounds_ctx_t;

/* The min/max of every key over rows[ib, ie) of a chunk. */
typedef struct { const agg_sel_bounds_ctx_t* c; const int64_t* rows; int64_t* mins; int64_t* maxs; } agg_sel_bounds_run_t;
/* One out-of-line copy, compiled as before, when it was also the staged
 * walk's body (address taken): the plain pass keeps its instance (the
 * staged pass reads key by key, agg_sel_bounds_staged).  noipa is
 * GCC-only (clang rejects the attribute). */
#if defined(__GNUC__) && !defined(__clang__)
#define AGG_ONE_COPY __attribute__((noipa))
#else
#define AGG_ONE_COPY __attribute__((noinline))
#endif
AGG_ONE_COPY static void agg_sel_bounds_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t ib, int64_t ie) {
    (void)wid;
    agg_sel_bounds_run_t* t = raw;
    const agg_sel_bounds_ctx_t* c = t->c;
    const int64_t* rows = t->rows;
    const int64_t base = v->base;
    for (uint32_t k = 0; k < c->n_keys; k++) {
        int64_t mn = t->mins[k], mx = t->maxs[k];
        const bool nullable = c->nullable[k];
        const int64_t null = c->nulls[k];
        const void* kd = v->key_data[k];
        /* the key's width switched once per run, not per row */
#define SEL_BOUNDS_LOOP(LD) for (int64_t i = ib; i < ie; i++) {               \
            int64_t val = (LD);                                                 \
            if (nullable && val == null) continue;                              \
            if (val < mn) mn = val;                                             \
            if (val > mx) mx = val;                                             \
        }
        switch (c->krr->key_types[k]) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP:        SEL_BOUNDS_LOOP(((const int64_t*)kd)[rows[i] - base]); break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: SEL_BOUNDS_LOOP(((const int32_t*)kd)[rows[i] - base]); break;
        case RAY_I16:                            SEL_BOUNDS_LOOP(((const int16_t*)kd)[rows[i] - base]); break;
        case RAY_U8: case RAY_BOOL:              SEL_BOUNDS_LOOP(((const uint8_t*)kd)[rows[i] - base]); break;
        default:                                 SEL_BOUNDS_LOOP(agg_vkey(v, c->krr->key_types, k, rows[i])); break;
        }
#undef SEL_BOUNDS_LOOP
        t->mins[k] = mn; t->maxs[k] = mx;
    }
}

/* The min/max of key k over rows[ib, ie), element 0 of data being row
 * `base` (the SEL_BOUNDS_LOOP fold of agg_sel_bounds_rows, for one key). */
static void agg_sel_bounds_fold(const agg_sel_bounds_ctx_t* c, uint32_t k, uint8_t attrs, const void* kd,
                                int64_t base, const int64_t* rows, int64_t ib, int64_t ie,
                                int64_t* mnp, int64_t* mxp) {
    int64_t mn = *mnp, mx = *mxp;
    const bool nullable = c->nullable[k];
    const int64_t null = c->nulls[k];
#define SEL_BOUNDS_LOOP(LD) for (int64_t i = ib; i < ie; i++) {               \
            int64_t val = (LD);                                                 \
            if (nullable && val == null) continue;                              \
            if (val < mn) mn = val;                                             \
            if (val > mx) mx = val;                                             \
        }
    switch (c->krr->key_types[k]) {
    case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP:        SEL_BOUNDS_LOOP(((const int64_t*)kd)[rows[i] - base]); break;
    case RAY_I32: case RAY_DATE: case RAY_TIME: SEL_BOUNDS_LOOP(((const int32_t*)kd)[rows[i] - base]); break;
    case RAY_I16:                            SEL_BOUNDS_LOOP(((const int16_t*)kd)[rows[i] - base]); break;
    case RAY_U8: case RAY_BOOL:              SEL_BOUNDS_LOOP(((const uint8_t*)kd)[rows[i] - base]); break;
    default: SEL_BOUNDS_LOOP(agg_read_key_i64_raw(c->krr->key_types[k], attrs, kd, rows[i] - base)); break;
    }
#undef SEL_BOUNDS_LOOP
    *mnp = mn; *mxp = mx;
}

/* Run r's logical value of a CONST / RLE view (ray_enc_runs' value),
 * inlined for the merges below, which walk a view's runs against sorted
 * rows directly (one compare per run that holds none of them). */
static inline int64_t agg_run_value(const ray_enc_vview_t* v, uint32_t r) {
    const ray_enc_kind_t k = v->k;
    uint64_t b;
    switch (k.T) {
    case 8:  b = v->run_vals.b[r]; break;
    case 16: b = v->run_vals.h[r]; break;
    case 32: b = v->run_vals.s[r]; break;
    default: return (int64_t)v->run_vals.q[r];
    }
    const uint64_t m = (1ull << k.T) - 1;
    return k.is_signed && (b >> (k.T - 1)) & 1 ? (int64_t)(b | ~m) : (int64_t)b;
}
/* The first run of v ending after local row x. */
static inline uint32_t agg_run_at(const ray_enc_vview_t* v, int64_t x) {
    uint32_t l = 0, h = v->n_runs;
    while (l < h) { uint32_t mid = l + (h - l) / 2; if ((int64_t)v->run_ends.h[mid] <= x) l = mid + 1; else h = mid; }
    return l;
}

/* The bounds over a run view: the runs holding some of the ascending rows
 * rows[i, j) (inside the view's vector, row 0 at vlo) fold their value, a
 * null skipped as the per-row fold skips it. */
static void agg_sel_bounds_runs(const ray_enc_vview_t* v, const int64_t* rows, int64_t i, int64_t j, int64_t vlo,
                                bool nullable, int64_t null, int64_t* mnp, int64_t* mxp) {
    int64_t mn = *mnp, mx = *mxp;
    for (uint32_t r = agg_run_at(v, rows[i] - vlo); r < v->n_runs && i < j; r++) {
        const int64_t e = vlo + v->run_ends.h[r];
        if (rows[i] >= e) continue;                     /* none of the rows in this run */
        const int64_t x = agg_run_value(v, r);
        if (!(nullable && x == null)) { if (x < mn) mn = x; if (x > mx) mx = x; }
        int64_t h = j;                                  /* past the run's rows */
        while (i < h) { int64_t mid = i + (h - i) / 2; if (rows[mid] < e) i = mid + 1; else h = mid; }
    }
    *mnp = mn; *mxp = mx;
}

/* agg_sel_bounds_rows of a staged context over the ascending rows[0, cn),
 * key by key: a plain key in place; a staged key's CONST / RLE vector
 * (ray_col_src_runview) on its runs (P3 Task 3b: no vector decoded); a
 * vector holding at most RAY_COL_POINT_ROWS of the rows point-read (open
 * decision D2: no vector decoded), any other vector, or one whose read
 * declines, through the key's cursor view.  A point-served value is never
 * null and lies inside its vector's statistics, so nests inside its
 * chunk's (the guard's rule); a run view under the guard checks its run
 * values against the chunk's statistics.  NULL or an owned error. */
static __attribute__((noinline)) ray_t* agg_sel_bounds_staged(agg_sel_bounds_run_t* t, agg_curs_t* cs, int64_t cn) {
    const agg_sel_bounds_ctx_t* c = t->c;
    const agg_rr_t* krr = c->krr;
    const int64_t* rows = t->rows;
    ray_enc_vview_t view;                               /* a key vector's runs: on the task's stack */
    for (uint32_t k = 0; k < c->n_keys; k++) {
        ray_col_src_t* src = krr->srcs[k];
        if (!src) {
            agg_sel_bounds_fold(c, k, krr->key_attrs[k], krr->plain[k], 0, rows, 0, cn, &t->mins[k], &t->maxs[k]);
            continue;
        }
        const bool point = krr->key_types[k] != RAY_SYM && ray_col_src_encoded(src);
        const bool runs = ray_col_src_encoded(src) && krr->key_types[k] != RAY_F32 && krr->key_types[k] != RAY_F64;
        for (int64_t i = 0; i < cn; ) {
            int64_t vlo, vhi;
            if (runs) {
                /* a CONST / RLE vector: the runs holding the rows fold their
                 * values, nothing decoded */
                ray_t* err = NULL;
                int vr = ray_col_src_runview(src, rows[i], &view, &vlo, &vhi, &err);
                if (vr < 0) return err;
                if (vr > 0) {
                    int64_t j = i + 1;
                    while (j < cn && rows[j] < vhi) j++;
                    agg_sel_bounds_runs(&view, rows, i, j, vlo, c->nullable[k], c->nulls[k], &t->mins[k], &t->maxs[k]);
                    i = j;
                    continue;
                }
            }
            if (point && ray_col_src_vec_span(src, rows[i], &vlo, &vhi)) {
                int64_t j = i + 1;
                while (j < cn && j - i <= RAY_COL_POINT_ROWS && rows[j] < vhi) j++;
                if (j - i <= RAY_COL_POINT_ROWS) {
                    int64_t x[RAY_COL_POINT_ROWS];
                    ray_t* err = NULL;
                    int pr = ray_col_src_points_i64(src, rows + i, j - i, x, &err);
                    if (pr < 0) return err;
                    if (pr > 0) {                    /* else declined: the cursor reads the vector */
                        for (int64_t q = 0; q < j - i; q++) {
                            if (x[q] < t->mins[k]) t->mins[k] = x[q];
                            if (x[q] > t->maxs[k]) t->maxs[k] = x[q];
                        }
                        i = j;
                        continue;
                    }
                }
            }
            const ray_col_range_t* v;
            ray_t* err = ray_col_cursor_at(&cs->cur[k], rows[i], &v);
            if (err) return err;
            int64_t end = v->start + v->n, j = i + 1;
            while (j < cn && rows[j] < end) j++;
            agg_sel_bounds_fold(c, k, v->attrs, v->data, v->start, rows, i, j, &t->mins[k], &t->maxs[k]);
            i = j;
        }
    }
    return NULL;
}

/* One task's min/max over its slice of selected-row space. */
static void agg_sel_bounds_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    agg_sel_bounds_ctx_t* c = raw;
    const agg_rr_t* krr = c->krr;
    int64_t rows[AGG_SEL_CHUNK];
    agg_view_t whole = agg_view_whole(krr);
    for (int64_t task = start; task < end; task++) {
        int64_t* mins = c->task_bounds + (2 * (size_t)task) * c->n_keys;
        int64_t* maxs = mins + c->n_keys;
        for (uint32_t k = 0; k < c->n_keys; k++) { mins[k] = INT64_MAX; maxs[k] = INT64_MIN; }
        int64_t begin = c->spans ? c->spans[2 * task] : c->n_sel / c->tasks * task;
        int64_t limit = c->spans ? c->spans[2 * task + 1]
                      : task + 1 == c->tasks ? c->n_sel : c->n_sel / c->tasks * (task + 1);
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, begin, limit);
        agg_sel_bounds_run_t t = { c, rows, mins, maxs };
        agg_curs_t cs; cs.cur = NULL;
        if (krr->staged) {
            if (ray_scan_fail_skip(krr->fail, begin)) continue;
            if (!agg_curs_init(&cs, krr)) { ray_scan_fail_note(krr->fail, begin, ray_error("oom", NULL)); continue; }
        }
        int64_t cn;
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0) {
            if (!krr->staged) { agg_sel_bounds_rows(&t, &whole, wid, 0, cn); continue; }
            ray_t* err = agg_sel_bounds_staged(&t, &cs, cn);
            if (err) { ray_scan_fail_note(krr->fail, begin, err); break; }
        }
        agg_curs_free(&cs, krr);
    }
}

/* The chunks of state 2 scanned by their selected rows (one task each),
 * folded into mins / maxs and marked 3.  False on a range error or cancel. */
static bool agg_sel_bounds_round(const agg_sel_bounds_ctx_t* bc, uint8_t* state, int64_t nch, int64_t spc,
                                 int64_t* spans, int64_t* tb, int64_t* mins, int64_t* maxs) {
    const uint32_t nsegs = ray_rowsel_meta(bc->sel)->n_segs;
    int64_t n = 0, rows = 0;
    for (int64_t ch = 0; ch < nch; ch++) {
        if (state[ch] != 2) continue;
        int64_t se = (ch + 1) * spc < (int64_t)nsegs ? (ch + 1) * spc : (int64_t)nsegs;
        spans[2 * n] = bc->sel_prefix[ch * spc];
        spans[2 * n + 1] = bc->sel_prefix[se];
        rows += spans[2 * n + 1] - spans[2 * n];
        state[ch] = 3;
        n++;
        for (uint32_t k = 0; k < bc->n_keys; k++)       /* clipped to the source's rows */
            ray_col_src_advise(bc->krr->srcs[k], ch * RAY_ENC_CHUNK, (ch + 1) * RAY_ENC_CHUNK);
    }
    if (n == 0) return true;
    agg_sel_bounds_ctx_t c = *bc;
    c.tasks = (uint32_t)n; c.spans = spans; c.task_bounds = tb;
    ray_pool_t* pool = ray_pool_get();
    bool par = pool && n > 1 && rows >= RAY_PARALLEL_THRESHOLD;
    ray_scan_fail_init(bc->krr->fail);
    if (par) ray_pool_dispatch_n(pool, agg_sel_bounds_fn, &c, (uint32_t)n);
    else agg_sel_bounds_fn(&c, 0, 0, n);
    ray_t* err = ray_scan_fail_finish(bc->krr->fail, par ? pool : NULL);
    if (err) { ray_error_free(err); return false; }
    for (int64_t t = 0; t < n; t++)
        for (uint32_t k = 0; k < bc->n_keys; k++) {
            int64_t mn = tb[(2 * (size_t)t) * bc->n_keys + k], mx = tb[(2 * (size_t)t + 1) * bc->n_keys + k];
            if (mn < mins[k]) mins[k] = mn;
            if (mx > maxs[k]) maxs[k] = mx;
        }
    return true;
}

/* The selected rows' key bounds of staged keys, exactly the full scan's
 * (so the plan, its route and its emit order are generation 0's), reading
 * only the chunks that can move them.  A chunk's directory min / max bound
 * its rows, so a chunk with no nulls whose statistics lie inside the bounds
 * found so far cannot change them: round 1 scans the selected rows of each
 * key's chunks holding the lowest minimum and the highest maximum and of
 * every chunk without statistics or with nulls; round 2 the chunks whose
 * statistics still reach outside.  Bounds only widen as chunks are read, so
 * no chunk skipped after round 2 could move them.  False (the caller scans
 * every selected row) unless every key is a single-file staged non-SYM
 * source with a directory; also on a scan error or cancel, so that the full
 * scan reports the lowest failure. */
static bool agg_sel_bounds_dir(const agg_sel_bounds_ctx_t* bc, int64_t* mins, int64_t* maxs) {
    const agg_rr_t* krr = bc->krr;
    const uint32_t nk = bc->n_keys;
    const ray_rowsel_t* m = ray_rowsel_meta(bc->sel);
    const int64_t nrows = m->nrows, nch = (nrows + RAY_ENC_CHUNK - 1) / RAY_ENC_CHUNK;
    const int64_t spc = RAY_ENC_CHUNK / RAY_MORSEL_ELEMS;      /* selection segments per chunk */
    ray_enc_chunk_stat_t st;
    if (nch < 1) return false;
    for (uint32_t k = 0; k < nk; k++)
        if (!krr->srcs[k] || krr->key_types[k] == RAY_SYM || krr->srcs[k]->len != nrows ||
            !ray_col_src_chunk_stat(krr->srcs[k], 0, &st))
            return false;
    ray_t* hdr;
    int64_t* spans = (int64_t*)scratch_alloc(&hdr, (size_t)nch * (2 + 2 * nk) * sizeof(int64_t) + (size_t)nch);
    if (!spans) return false;
    int64_t* tb = spans + 2 * nch;
    uint8_t* state = (uint8_t*)(tb + 2 * (size_t)nch * nk);   /* 0 no selected row, 1 skippable, 2 to scan, 3 scanned */
    int64_t lo_ch[16], hi_ch[16], lo_v[16], hi_v[16];
    for (uint32_t k = 0; k < nk; k++) { lo_ch[k] = hi_ch[k] = -1; lo_v[k] = INT64_MAX; hi_v[k] = INT64_MIN; }
    for (int64_t ch = 0; ch < nch; ch++) {
        int64_t se = (ch + 1) * spc < (int64_t)m->n_segs ? (ch + 1) * spc : (int64_t)m->n_segs;
        state[ch] = bc->sel_prefix[se] > bc->sel_prefix[ch * spc];
        for (uint32_t k = 0; k < nk && state[ch]; k++) {
            (void)ray_col_src_chunk_stat(krr->srcs[k], ch, &st);
            /* Conservative: a chunk with nulls is always scanned.  Its
             * statistics bound only its non-null rows, which is all the
             * scan folds when the plan holds the key nullable (the usual
             * case: the column's null count makes it so); scanning keeps
             * the bounds exact without relying on that, and such chunks
             * are few. */
            if (!st.has_minmax || st.null_count) { state[ch] = 2; continue; }
            if (st.min < lo_v[k]) { lo_v[k] = st.min; lo_ch[k] = ch; }
            if (st.max > hi_v[k]) { hi_v[k] = st.max; hi_ch[k] = ch; }
        }
    }
    for (uint32_t k = 0; k < nk; k++) {
        if (lo_ch[k] >= 0) state[lo_ch[k]] = 2;
        if (hi_ch[k] >= 0) state[hi_ch[k]] = 2;
        mins[k] = INT64_MAX; maxs[k] = INT64_MIN;
    }
    bool ok = agg_sel_bounds_round(bc, state, nch, spc, spans, tb, mins, maxs);
    for (int64_t ch = 0; ok && ch < nch; ch++) {
        if (state[ch] != 1) continue;
        for (uint32_t k = 0; k < nk; k++) {
            (void)ray_col_src_chunk_stat(krr->srcs[k], ch, &st);
            if (st.min < mins[k] || st.max > maxs[k]) { state[ch] = 2; break; }
        }
    }
    if (ok) ok = agg_sel_bounds_round(bc, state, nch, spc, spans, tb, mins, maxs);
    scratch_free(hdr);
    return ok;
}

/* Every selected row's key bounds, scanned (bc->tasks tasks over the
 * selection).  False on OOM (no *err) or a range error (*err owned, the
 * lowest failure). */
static bool agg_sel_bounds_scan(agg_sel_bounds_ctx_t* bc, int64_t* mins, int64_t* maxs, ray_t** err) {
    const agg_rr_t* krr = bc->krr;
    const uint32_t nk = bc->n_keys, tasks = bc->tasks;
    for (uint32_t k = 0; k < nk; k++) { mins[k] = INT64_MAX; maxs[k] = INT64_MIN; }
    ray_t* tb_hdr;
    bc->task_bounds = (int64_t*)scratch_alloc(&tb_hdr, 2u * (size_t)tasks * nk * sizeof(int64_t));
    if (!bc->task_bounds) return false;
    if (krr->staged) {
        ray_scan_fail_init(krr->fail);
        for (uint32_t k = 0; k < nk; k++)
            if (krr->srcs[k]) ray_col_src_advise(krr->srcs[k], 0, ray_rowsel_meta(bc->sel)->nrows);
    }
    ray_pool_t* pool = ray_pool_get();
    if (tasks > 1) ray_pool_dispatch_n(pool, agg_sel_bounds_fn, bc, tasks);
    else agg_sel_bounds_fn(bc, 0, 0, 1);
    if (krr->staged && (*err = ray_scan_fail_finish(krr->fail, tasks > 1 ? pool : NULL)) != NULL) {
        scratch_free(tb_hdr);
        bc->task_bounds = NULL;
        return false;
    }
    for (uint32_t t = 0; t < tasks; t++)
        for (uint32_t k = 0; k < nk; k++) {
            int64_t mn = bc->task_bounds[(2 * (size_t)t) * nk + k];
            int64_t mx = bc->task_bounds[(2 * (size_t)t + 1) * nk + k];
            if (mn < mins[k]) mins[k] = mn;
            if (mx > maxs[k]) maxs[k] = mx;
        }
    scratch_free(tb_hdr);
    bc->task_bounds = NULL;
    return true;
}

/* A subset of a bounds context's keys as a context of its own: the same
 * selection, the keys' sources (pointers, never copies) and plain
 * pointers, no aggregate input. */
typedef struct {
    agg_rr_t       rr;
    ray_col_src_t* srcs[16];
    const void*    plain[16];
    uint8_t        esz[16], attrs[16];
    int8_t         types[16];
    bool           nullable[16];
    int64_t        nulls[16];
} agg_sel_keys_t;
static void agg_sel_keys(const agg_sel_bounds_ctx_t* bc, const uint32_t* ks, uint32_t n,
                         agg_sel_keys_t* o, agg_sel_bounds_ctx_t* sub) {
    const agg_rr_t* krr = bc->krr;
    bool staged = false;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t k = ks[i];
        o->srcs[i] = krr->srcs[k];
        o->plain[i] = krr->plain[k];
        o->esz[i] = krr->esz[k];
        o->attrs[i] = krr->key_attrs[k];
        o->types[i] = krr->key_types[k];
        o->nullable[i] = bc->nullable[k];
        o->nulls[i] = bc->nulls[k];
        staged |= o->srcs[i] != NULL;
    }
    o->rr = (agg_rr_t){ .srcs = o->srcs, .plain = o->plain, .esz = o->esz, .key_types = o->types,
                        .key_attrs = o->attrs, .n_src = n, .n_keys = n, .n_aggs = 0,
                        .fail = krr->fail, .staged = staged };
    *sub = *bc;
    sub->krr = &o->rr; sub->nullable = o->nullable; sub->nulls = o->nulls; sub->n_keys = n;
    sub->spans = NULL; sub->task_bounds = NULL;
}

/* The selected rows' exact key bounds of a staged context whose keys are
 * read two ways, without scanning what cannot change the plan.  The keys
 * agg_sel_bounds_dir cannot prune (plain, SYM, parted, without a
 * directory) are scanned first.  When their ranges alone exceed the dense
 * limit (or one has no range), the plan is impossible whatever the other
 * keys hold (every range is at least 1): 2, nothing more read.  Otherwise
 * the single-file generation-2 non-SYM keys take the directory path, as a
 * key set of their own: 1, mins / maxs exact for every key, so the plan,
 * its route and its emit order are the full scan's.  0: not such a key
 * set (every key one way), or a scan error or cancel (the caller's full
 * scan then reports the lowest failure). */
static int agg_sel_bounds_split(const agg_sel_bounds_ctx_t* bc, int64_t* mins, int64_t* maxs) {
    const agg_rr_t* krr = bc->krr;
    const uint32_t nk = bc->n_keys;
    const int64_t nrows = ray_rowsel_meta(bc->sel)->nrows;
    uint32_t dk[16], ok[16], nd = 0, no = 0;
    ray_enc_chunk_stat_t st;
    for (uint32_t k = 0; k < nk; k++) {
        if (krr->srcs[k] && krr->key_types[k] != RAY_SYM && krr->srcs[k]->len == nrows &&
            ray_col_src_chunk_stat(krr->srcs[k], 0, &st)) dk[nd++] = k;
        else ok[no++] = k;
    }
    if (nd == 0 || no == 0) return 0;
    agg_sel_keys_t ko, kd;
    agg_sel_bounds_ctx_t co, cd;
    int64_t mn[16], mx[16];
    agg_sel_keys(bc, ok, no, &ko, &co);
    ray_t* err = NULL;
    if (!agg_sel_bounds_scan(&co, mn, mx, &err)) { if (err) ray_error_free(err); return 0; }
    const int64_t limit = bc->n_sel < (int64_t)UINT32_MAX ? bc->n_sel : (int64_t)UINT32_MAX;
    int64_t total = 1;
    for (uint32_t i = 0; i < no; i++) {                 /* agg_dense_range's ranges */
        int64_t rng;
        if (mx[i] < mn[i]) {
            if (!co.nullable[i]) return 2;
            rng = 1;
        } else {
            uint64_t span = (uint64_t)mx[i] - (uint64_t)mn[i];
            if (span >= (uint64_t)INT64_MAX - (uint64_t)co.nullable[i]) return 2;
            rng = (int64_t)span + 1 + co.nullable[i];
        }
        if (total > limit / rng) return 2;
        total *= rng;
    }
    for (uint32_t i = 0; i < no; i++) { mins[ok[i]] = mn[i]; maxs[ok[i]] = mx[i]; }
    agg_sel_keys(bc, dk, nd, &kd, &cd);
    if (!agg_sel_bounds_dir(&cd, mn, mx)) return 0;
    for (uint32_t i = 0; i < nd; i++) { mins[dk[i]] = mn[i]; maxs[dk[i]] = mx[i]; }
    return 1;
}

/* A probe that can only prove the dense plan impossible, for staged keys:
 * the key values of AGG_SEL_PROBE_ROWS evenly spaced selected rows (in
 * place or by point reads; a declined read skips its row, so nothing is
 * decoded) bound the selected rows' exact ranges from
 * below, so when their ranges already exceed the dense limit (as
 * agg_sel_bounds_split judges them, nullability included) the exact
 * bounds do too, and the scan of every selected row is skipped: the
 * plan, the route and the emit order stay those of the exact bounds.
 * True: impossible.  False: undecided (a read error or OOM included; the
 * exact scan reports it). */
#define AGG_SEL_PROBE_ROWS (RAY_MORSEL_ELEMS / 16)
/* True when the key ranges mn / mx (a subset of the selected rows' values)
 * already exceed the dense limit, as agg_sel_bounds_split judges them
 * (agg_dense_range's ranges, nullability included). */
static bool agg_sel_ranges_exceed(const agg_sel_bounds_ctx_t* bc, const int64_t* mn, const int64_t* mx) {
    const int64_t limit = bc->n_sel < (int64_t)UINT32_MAX ? bc->n_sel : (int64_t)UINT32_MAX;
    int64_t total = 1;
    for (uint32_t k = 0; k < bc->n_keys; k++) {
        if (mx[k] < mn[k]) continue;                   /* no value seen: at least 1 */
        uint64_t span = (uint64_t)mx[k] - (uint64_t)mn[k];
        if (span >= (uint64_t)INT64_MAX - (uint64_t)bc->nullable[k]) return true;
        int64_t rng = (int64_t)span + 1 + bc->nullable[k];
        if (total > limit / rng) return true;
        total *= rng;
    }
    return false;
}
static _Atomic(uint64_t) g_sel_probe_hits;   /* test hook: plans the probe proved impossible */
uint64_t ray_agg_test_sel_probe_hits(void) { return atomic_load_explicit(&g_sel_probe_hits, memory_order_relaxed); }
static bool agg_sel_bounds_impossible(const agg_sel_bounds_ctx_t* bc) {
    const agg_rr_t* krr = bc->krr;
    const uint32_t nk = bc->n_keys;
    if (nk < 1 || nk > 16 || bc->n_sel < 2) return false;
    int64_t mn[16], mx[16];
    for (uint32_t k = 0; k < nk; k++) { mn[k] = INT64_MAX; mx[k] = INT64_MIN; }
    const int64_t np = bc->n_sel < AGG_SEL_PROBE_ROWS ? bc->n_sel : AGG_SEL_PROBE_ROWS;
    bool ok = true;
    for (int64_t i = 0; i < np && ok; i++) {
        int64_t o = bc->n_sel / np * i, row;
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, bc->sel, bc->sel_prefix, o, o + 1);
        if (agg_sel_cursor_next(&cur, &row) != 1) { ok = false; break; }
        for (uint32_t k = 0; k < nk; k++) {
            /* in place, or a point read; a declined one skips the row for
             * this key (fewer rows only weaken the bound: nothing decodes) */
            int64_t x;
            if (!krr->srcs[k]) x = agg_read_key_i64_raw(krr->key_types[k], krr->key_attrs[k], krr->plain[k], row);
            else {
                ray_t* err = NULL;
                int pr = krr->srcs[k]->type == RAY_SYM ? agg_src_points(krr->srcs[k], &row, 1, &x, &err)
                                                       : ray_col_src_point_i64(krr->srcs[k], row, &x, &err);
                if (pr < 0) { ray_error_free(err); ok = false; break; }
                if (pr == 0) continue;
            }
            if (bc->nullable[k] && x == bc->nulls[k]) continue;
            if (x < mn[k]) mn[k] = x;
            if (x > mx[k]) mx[k] = x;
        }
    }
    if (!ok) return false;
    if (agg_sel_ranges_exceed(bc, mn, mx)) return true;
    /* Undecided: the selected rows of the chunks holding each SYM key's
     * lowest minimum and highest maximum in the directory (its membership
     * bounds; a SYM key has no directory route to its exact bounds) are
     * scanned too.  The statistics only choose the chunks; the bound is
     * still the values read, so it stays a subset of the exact scan's
     * whatever they say.  A key whose extremes are a few rare values (SYM
     * ids interned early) is proved here, where evenly spaced rows miss
     * them. */
    const ray_rowsel_t* m = ray_rowsel_meta(bc->sel);
    const int64_t nrows = m->nrows, nch = (nrows + RAY_ENC_CHUNK - 1) / RAY_ENC_CHUNK;
    const int64_t spc = RAY_ENC_CHUNK / RAY_MORSEL_ELEMS;      /* selection segments per chunk */
    if (nch < 2) return false;                                 /* one chunk: the exact scan */
    /* the SYM keys with a directory (the others' exact bounds take the
     * directory, agg_sel_bounds_dir / _split, which reads these chunks
     * anyway); none (every generation-0 key): nothing allocated */
    uint32_t sk[16], nsk = 0;
    ray_enc_chunk_stat_t st;
    for (uint32_t k = 0; k < nk; k++)
        if (krr->srcs[k] && krr->key_types[k] == RAY_SYM && krr->srcs[k]->len == nrows &&
            ray_col_src_chunk_stat(krr->srcs[k], 0, &st)) sk[nsk++] = k;
    if (nsk == 0) return false;
    ray_t* hdr;
    int64_t* spans = (int64_t*)scratch_alloc(&hdr, (size_t)nch * (2 + 2 * nk) * sizeof(int64_t) + (size_t)nch);
    if (!spans) return false;
    int64_t* tb = spans + 2 * nch;
    uint8_t* state = (uint8_t*)(tb + 2 * (size_t)nch * nk);
    memset(state, 0, (size_t)nch);
    int64_t probe_rows = 0;                                    /* selected rows of the marked chunks */
    uint32_t nread = 0;
    for (uint32_t i = 0; i < nsk; i++) {
        const uint32_t k = sk[i];
        int64_t lo_ch = -1, hi_ch = -1, lo_v = INT64_MAX, hi_v = INT64_MIN;
        for (int64_t ch = 0; ch < nch; ch++) {
            int64_t se = (ch + 1) * spc < (int64_t)m->n_segs ? (ch + 1) * spc : (int64_t)m->n_segs;
            if (bc->sel_prefix[se] == bc->sel_prefix[ch * spc]) continue;     /* no selected row */
            if (!ray_col_src_chunk_stat(krr->srcs[k], ch, &st) || !st.has_minmax) continue;
            if (st.min < lo_v) { lo_v = st.min; lo_ch = ch; }
            if (st.max > hi_v) { hi_v = st.max; hi_ch = ch; }
        }
        if (lo_ch < 0) continue;
        sk[nread++] = k;
        const int64_t mk[2] = { lo_ch, hi_ch };
        for (int e = 0; e < 2; e++) {
            const int64_t ch = mk[e];
            if (state[ch] == 2) continue;
            state[ch] = 2;
            int64_t se = (ch + 1) * spc < (int64_t)m->n_segs ? (ch + 1) * spc : (int64_t)m->n_segs;
            probe_rows += bc->sel_prefix[se] - bc->sel_prefix[ch * spc];
        }
    }
    /* The probe reads the marked chunks' selected rows; undecided, the
     * exact scan then reads every selected row again, so a miss costs
     * probe_rows / n_sel more.  At most a quarter of the selected rows
     * keeps a miss under a quarter of the exact scan, while a hit saves
     * the other three quarters and more (q13: 2 of 153 chunks).  Those
     * keys alone are read (a key set of their own, agg_sel_keys). */
    bool proved = false;
    if (nread > 0 && probe_rows * 4 <= bc->n_sel) {
        agg_sel_keys_t ko;
        agg_sel_bounds_ctx_t co;
        agg_sel_keys(bc, sk, nread, &ko, &co);
        int64_t cm[16], cx[16];
        for (uint32_t i = 0; i < nread; i++) { cm[i] = INT64_MAX; cx[i] = INT64_MIN; }
        if (agg_sel_bounds_round(&co, state, nch, spc, spans, tb, cm, cx)) {
            int64_t am[16], ax[16];
            for (uint32_t k = 0; k < nk; k++) { am[k] = mn[k]; ax[k] = mx[k]; }
            for (uint32_t i = 0; i < nread; i++) {
                if (cm[i] < am[sk[i]]) am[sk[i]] = cm[i];
                if (cx[i] > ax[sk[i]]) ax[sk[i]] = cx[i];
            }
            proved = agg_sel_ranges_exceed(bc, am, ax);
        }
    }
    scratch_free(hdr);
    return proved;
}

static bool agg_dense_plan_sel(ray_t** key_cols, const agg_rr_t* krr, uint32_t n_keys, int64_t n_sel,
                               ray_t* sel, const int64_t* sel_prefix,
                               dense_plan_t* out, ray_t** err) {
    *err = NULL;
    out->ok = false;
    out->n_keys = n_keys;
    out->compacted = false;
    out->wide = false;
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
    void* pre = scratch_alloc(&pre_hdr, 2u * (size_t)n_keys * 8);   /* mins,maxs */
    if (!pre) return false;
    int64_t* mins = (int64_t*)pre;
    int64_t* maxs = mins + n_keys;
    for (uint32_t k = 0; k < n_keys; k++) { mins[k] = INT64_MAX; maxs[k] = INT64_MIN; }

    ray_pool_t* pool = ray_pool_get();
    uint32_t tasks = pool && n_sel >= RAY_PARALLEL_THRESHOLD ? ray_pool_total_workers(pool) * 4 : 1;
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    if ((int64_t)tasks > n_sel) tasks = (uint32_t)n_sel;
    agg_sel_bounds_ctx_t bc = { .krr = krr,
        .nullable = out->nullable, .nulls = out->nulls, .n_keys = n_keys,
        .sel = sel, .sel_prefix = sel_prefix, .n_sel = n_sel, .tasks = tasks };
    /* staged: the directory prunes what it can (every key, or the keys it
     * serves after the others, agg_sel_bounds_split); else every selected
     * row is scanned */
    if (krr->staged && agg_sel_bounds_impossible(&bc)) {
        atomic_fetch_add_explicit(&g_sel_probe_hits, 1, memory_order_relaxed);
        scratch_free(pre_hdr);
        return false;
    }
    int split = krr->staged ? agg_sel_bounds_split(&bc, mins, maxs) : 0;
    if (split == 2) { scratch_free(pre_hdr); return false; }     /* dense is impossible */
    if (split == 0 && (!krr->staged || !agg_sel_bounds_dir(&bc, mins, maxs)) &&
        !agg_sel_bounds_scan(&bc, mins, maxs, err)) {
        scratch_free(pre_hdr);
        return false;
    }
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

/* agg_tuple_hash of a group's stored key words (the same FNV-1a + fmix64):
 * a staged small hash keys its groups by words, never by first rows. */
static inline uint64_t agg_words_hash(const int64_t* w, uint32_t n_keys) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < n_keys; k++) { h ^= (uint64_t)w[k]; h *= 1099511628211ULL; }
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
    const agg_rr_t*     rr;       /* sources; staged ones are read by range */
    const agg_view_t*   whole;
    bool                runs;     /* COUNT only over one staged key: its run views add to the slots */
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

/* Threshold of a top-N from the union of per-range candidate sets.  The
 * union of every range's N best contains the global N best, so when it
 * holds more than N values the N-th in keep direction is the threshold; when
 * it holds exactly N (one range) the threshold is the worst candidate.  No
 * threshold when the whole population fits in N. */
static bool agg_topn_union_threshold(const double* cand, int64_t nc, int64_t total,
                                     const ray_group_emit_filter_t* ef, double* thr) {
    if (ef->top_count_take <= 0 || total <= ef->top_count_take || nc <= 0) return false;
    if (nc > ef->top_count_take) return agg_topn_threshold(cand, nc, ef, thr);
    double worst = cand[0];
    for (int64_t i = 1; i < nc; i++)
        if (ef->desc ? cand[i] < worst : cand[i] > worst) worst = cand[i];
    *thr = worst;
    return true;
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
        case RAY_F64: case RAY_F32: case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP:
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
            case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: v = (double)((const int64_t*)ray_data(cell))[0]; break;
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
static inline int64_t agg_dense_slot(const agg_dense_ctx_t* c, const agg_view_t* v, int64_t r, bool compacted) {
    int64_t slot = 0;
    const int64_t i = r - v->base;
    if (compacted) {
        for (uint32_t k = 0; k < c->n_keys; k++)
            slot += agg_dense_component(c->dp, k, agg_read_key_i64_raw(c->key_cols[k]->type, v->key_attrs[k], v->key_data[k], i)) * c->dp->strides[k];
    } else {
        for (uint32_t k = 0; k < c->n_keys; k++)
            slot += agg_dense_component_raw(c->dp, k, agg_read_key_i64_raw(c->key_cols[k]->type, v->key_attrs[k], v->key_data[k], i)) * c->dp->strides[k];
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

/* Phase A, sel mode: the slots and the accumulation of one run of selected
 * rows rows[ib, ie) read through view v. */
typedef struct {
    agg_dense_ctx_t*   c;
    agg_dense_local_t* loc;
    const int64_t*     rows;
    uint32_t*          gid;
    agg_sel_scratch_t* sc;
} agg_dense_sel_run_t;
static void agg_dense_sel_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t ib, int64_t ie) {
    (void)wid;
    agg_dense_sel_run_t* t = raw;
    agg_dense_ctx_t* c = t->c;
    agg_dense_local_t* loc = t->loc;
    if (loc->oom) return;
    /* Locals: the init calls below would otherwise reload every field. */
    const int64_t* rows = t->rows;
    uint32_t* gid = t->gid;
    int64_t* fr = loc->first_row;
    char* states = loc->states;
    const bool compacted = c->dp->compacted;
    for (int64_t i = ib; i < ie; i++) {
        int64_t r = rows[i];
        int64_t slot = agg_dense_slot(c, v, r, compacted);   /* provably in [0,total_slots) */
        gid[i - ib] = (uint32_t)slot;
        if (agg_dense_first(loc, slot)) {
            for (uint32_t a = 0; a < c->n_aggs; a++)
                c->vts[a]->init(states + (size_t)slot * c->block + c->off[a]);
            fr[slot] = r;
        }
        if (r < fr[slot]) fr[slot] = r;
    }
    if (agg_sel_accum_chunk(&c->vd, v, t->sc, states, gid, rows + ib, ie - ib, false) != 0)
        loc->oom = 1;
}

static void agg_dense_phaseA_fn(void* vctx, const agg_view_t* v, uint32_t wid, int64_t start, int64_t end);

/* Phase A, sel mode: the SELECTED rows in [start,end) (selected-row
 * space), segment by segment.  The rows of a mixed segment are decoded
 * into chunks of ORIGINAL rows: dense slots computed, agg values gathered
 * per chunk into reused scratch, batch-updated (R-SPARSE through the
 * task's cursors when a source is staged).  A run of segments the
 * selection takes whole is a dense row range, accumulated as without a
 * selection (agg_dense_phaseA_fn, by pieces when staged).  The buffered
 * rows go first, so every slot still sees its rows in ascending order. */
static void agg_dense_phaseA_sel(agg_dense_ctx_t* c, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_local_t* loc = &c->locals[wid];
    if (loc->oom || end <= start) return;
    int64_t rows[AGG_SEL_CHUNK];
    uint32_t gid[AGG_SEL_CHUNK];
    agg_sel_scratch_t sc;
    if (!agg_sel_scratch_init(&sc, c->n_aggs)) { loc->oom = 1; return; }
    const agg_rr_t* rr = c->rr;
    agg_curs_t cs; cs.cur = NULL;
    if (rr->staged) {
        if (ray_scan_fail_skip(rr->fail, start)) { agg_sel_scratch_free(&sc); return; }
        if (!agg_curs_init(&cs, rr)) { loc->oom = 1; agg_sel_scratch_free(&sc); return; }
    }
    agg_sel_cursor_t cur;                               /* for the read-ahead bound only */
    agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, start, end);
    agg_dense_sel_run_t t = { c, loc, rows, gid, &sc };
    const ray_rowsel_t* m = ray_rowsel_meta(c->sel);
    const uint8_t* flags = ray_rowsel_flags(c->sel);
    const uint32_t* offs = ray_rowsel_offsets(c->sel);
    const uint16_t* idx = ray_rowsel_idx(c->sel);
    const int64_t* pre = c->sel_prefix;
    const int64_t M = RAY_MORSEL_ELEMS;
    uint32_t s = 0, hs = m->n_segs;                     /* the segment holding selected row `start` */
    while (s < hs) { uint32_t mid = s + (hs - s) / 2; if (pre[mid + 1] <= start) s = mid + 1; else hs = mid; }
    int64_t nb = 0;                                     /* buffered rows of mixed segments */
    ray_t* err = NULL;
    bool failed = false;                                /* a dense range's read failed (noted) */
    #define AGG_SEL_FLUSH() do {                                                    \
        if (nb > 0 && !loc->oom) {                                                  \
            ray_pool_scan_at(m->nrows, rows[0], cur.row_end);                       \
            if (!rr->staged) agg_dense_sel_rows(&t, c->whole, wid, 0, nb);          \
            else err = agg_idx_walk(rr, &cs, rows, 0, nb, &t, agg_dense_sel_rows, wid); \
        }                                                                           \
        nb = 0;                                                                     \
    } while (0)
    while (s < m->n_segs && pre[s] < end && !err && !failed && !loc->oom) {
        if (flags[s] == RAY_SEL_NONE) { s++; continue; }
        const int64_t seg_sel = pre[s + 1] - pre[s];
        const int64_t p0 = start > pre[s] ? start - pre[s] : 0;
        const int64_t p1 = end - pre[s] < seg_sel ? end - pre[s] : seg_sel;
        const int64_t base = (int64_t)s * M;
        if (flags[s] == RAY_SEL_ALL) {
            /* the whole segments that follow join the range, up to the
             * kernel's 8192 rows */
            int64_t r0 = base + p0, r1 = base + p1;
            s++;
            while (p1 == seg_sel && s < m->n_segs && flags[s] == RAY_SEL_ALL && pre[s] < end &&
                   r1 == (int64_t)s * M && r1 - r0 < 8192) {
                int64_t q1 = end - pre[s] < pre[s + 1] - pre[s] ? end - pre[s] : pre[s + 1] - pre[s];
                int64_t e1 = (int64_t)s * M + q1;
                if (e1 - r0 > 8192) break;
                r1 = e1;
                if (q1 < pre[s + 1] - pre[s]) { s++; break; }
                s++;
            }
            AGG_SEL_FLUSH();
            if (err || loc->oom) break;
            if (agg_cancelled()) break;
            ray_pool_scan_at(m->nrows, r0, cur.row_end);
            if (!rr->staged) agg_dense_phaseA_fn(c, c->whole, wid, r0, r1);
            else if (!agg_walk_cs(rr, &cs, c, agg_dense_phaseA_fn, wid, r0, r1, start)) failed = true;
            continue;
        }
        const uint16_t* si = idx + offs[s];
        for (int64_t q = p0; q < p1 && !err; q++) {
            rows[nb++] = base + si[q];
            if (nb == AGG_SEL_CHUNK) AGG_SEL_FLUSH();
        }
        s++;
    }
    if (!err && !failed) AGG_SEL_FLUSH();
    #undef AGG_SEL_FLUSH
    if (err) ray_scan_fail_note(rr->fail, start, err);
    agg_curs_free(&cs, rr);
    agg_sel_scratch_free(&sc);
}

/* Phase A: per-worker dense accumulate over chunk [start,end) (at most
 * 8192 rows) read through view v: the whole columns, or one piece. */
static void agg_dense_phaseA_fn(void* vctx, const agg_view_t* v, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_ctx_t* c = vctx;
    agg_dense_local_t* loc = &c->locals[wid];
    if (loc->oom) return;
    int64_t n = end - start;
    if (n <= 0) return;
    const int64_t kbase = v->base;

    uint32_t cgid[8192];

    /* Hoist the per-row key-type dispatch out of the hot loop.  agg_dense_slot
     * calls agg_read_key_i64 — a switch(col->type) — on every row, and for SYM
     * keys ray_read_sym adds a second per-row switch on the code width.  Both
     * are loop-invariant.  For the common single-key case, branch ONCE on
     * (type, width) and run a tight typed load loop the compiler can vectorize;
     * this is the dominant cost of a low-card group-by (a 7-group count was
     * ~70% here).  Multi-key keeps the generic composite path. */
    if (c->n_keys == 1) {
        const void* kd = v->key_data[0];
        int64_t  kmin = c->dp->mins[0];
        bool nullable = c->dp->nullable[0];   /* one key: its stride is 1 */
        int64_t null = c->dp->nulls[0], null_slot = c->dp->ranges[0] - 1;
        int64_t* fr = loc->first_row;
        uint32_t* cg = cgid;
        /* Only the nullable instance tests for the null (a ragged
         * partition's null segment makes the key nullable); the slot is
         * v - kmin, a single key's stride being 1. */
        #define DENSE_SLOT1_LAZY(LD, SLOT)                                      \
            for (int64_t r = start; r < end; r++) {                        \
                int64_t v = (int64_t)(LD);                               \
                int64_t slot = (SLOT);                                    \
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
            } else if (!nullable) { DENSE_SLOT1_LAZY(LD, v - kmin); }     \
            else { DENSE_SLOT1_LAZY(LD, v == null ? null_slot : v - kmin); } \
        } while (0)
        switch (c->key_cols[0]->type) {
            case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: DENSE_SLOT1(((const int64_t*)kd)[r - kbase]); break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: DENSE_SLOT1(((const int32_t*)kd)[r - kbase]); break;
            case RAY_I16: DENSE_SLOT1(((const int16_t*)kd)[r - kbase]); break;
            case RAY_U8: case RAY_BOOL: DENSE_SLOT1(((const uint8_t*)kd)[r - kbase]); break;
            case RAY_SYM:
                switch (v->key_attrs[0] & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8:  DENSE_SLOT1(((const uint8_t*)kd)[r - kbase]);  break;
                    case RAY_SYM_W16: DENSE_SLOT1(((const uint16_t*)kd)[r - kbase]); break;
                    case RAY_SYM_W32: DENSE_SLOT1(((const uint32_t*)kd)[r - kbase]); break;
                    default:          DENSE_SLOT1(((const int64_t*)kd)[r - kbase]);  break; /* W64 */
                }
                break;
            default:
                for (int64_t r = start; r < end; r++) {
                    int64_t slot = agg_dense_slot(c, v, r, false);   /* single key: never compacted */
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
                int64_t slot = agg_dense_slot(c, v, r, COMPACTED);   /* provably in [0,total_slots) */ \
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
            const void* vx = (const char*)v->val_data[a]  + (size_t)(start - kbase) * c->val_esz[a];
            const void* vy = (const char*)v->val2_data[a] + (size_t)(start - kbase) * c->val2_esz[a];
            ray_valid_t valid_x = { vx, c->val_types[a],  c->val_hasnull[a] };
            ray_valid_t valid_y = { vy, c->val2_types[a], c->val2_hasnull[a] };
            c->vts[a]->update_batch2(loc->states + c->off[a], c->block, cgid,
                                     vx, vy, &valid_x, &valid_y, n, NULL);
        } else {
            const void* vals = (v->val_data[a])
                ? (const void*)((const char*)v->val_data[a] + (size_t)(start - kbase) * c->val_esz[a])
                : NULL;
            ray_valid_t valid = { vals, c->val_types[a],
                                  v->val_data[a] ? c->val_hasnull[a] : false };
            c->vts[a]->update_batch(loc->states + c->off[a], c->block,
                                    cgid, vals, &valid, n, NULL);
        }
    }
}

/* Phase A on the key's runs (ctx->runs: every aggregate COUNT, one
 * staged key).  A CONST / RLE key vector without bitmap nulls
 * (ray_col_src_runview) adds each run's rows to its slot: the run's length
 * inside the task's rows, or the number of the selection's rows inside the
 * run; the slot is the per-row body's (agg_dense_component_raw: one key,
 * never compacted), the first row the run's first counted row.  Every
 * other vector is read and counted per row by the bodies above, as before
 * (D6).  A slot outside the slab is `corrupt` (an internal plan check).
 * Exact for any rows a task holds: nothing
 * depends on the task cut. */
typedef struct {
    agg_dense_ctx_t*   c;
    agg_dense_local_t* loc;
    int64_t            vlo;          /* source row of the view's row 0 */
    int64_t            r0, r1;       /* dense rows [r0, r1); idx == NULL */
    const uint16_t*    idx;          /* else rows ibase + idx[ii, ni), ascending */
    int64_t            ibase, ii, ni;
    bool               bad;          /* a run value outside the plan */
} agg_dense_runs_t;

static inline void agg_dense_run_add(agg_dense_runs_t* t, int64_t value, int64_t first, int64_t cnt) {
    const agg_dense_ctx_t* c = t->c;
    agg_dense_local_t* loc = t->loc;
    const int64_t slot = agg_dense_component_raw(c->dp, 0, value);
    if (slot < 0 || slot >= loc->slots) { t->bad = true; return; }
    int64_t* fr = loc->first_row;
    char* st = loc->states + (size_t)slot * c->block;
    if (loc->eager) { if (first < fr[slot]) fr[slot] = first; }
    else if (agg_dense_first(loc, slot)) {
        for (uint32_t a = 0; a < c->n_aggs; a++) c->vts[a]->init(st + c->off[a]);
        fr[slot] = first;
    } else if (first < fr[slot]) fr[slot] = first;
    for (uint32_t a = 0; a < c->n_aggs; a++) {          /* COUNT's state: one int64_t count */
        int64_t n; memcpy(&n, st + c->off[a], sizeof n);
        n += cnt;
        memcpy(st + c->off[a], &n, sizeof n);
    }
}
/* ray_enc_runs callback: the run's rows inside dense rows [r0, r1) */
static void agg_dense_run_rows(void* raw, int64_t value, int32_t lo, int32_t hi) {
    agg_dense_runs_t* t = raw;
    int64_t a = t->vlo + lo, b = t->vlo + hi;
    if (a < t->r0) a = t->r0;
    if (b > t->r1) b = t->r1;
    if (a < b) agg_dense_run_add(t, value, a, b - a);
}
/* The selection's rows ibase + idx[ii, ni) inside the view's vector,
 * merged with its runs: each run holding some adds their count. */
static void agg_dense_runs_idx(agg_dense_runs_t* t, const ray_enc_vview_t* v) {
    const uint16_t* idx = t->idx;
    const int64_t ni = t->ni, off = t->vlo - t->ibase;  /* local run end e <=> index e + off */
    int64_t i = t->ii;
    for (uint32_t r = agg_run_at(v, idx[i] - off); r < v->n_runs && i < ni && !t->bad; r++) {
        const int64_t e = (int64_t)v->run_ends.h[r] + off;
        if (idx[i] >= e) continue;                       /* none of the rows in this run */
        const int64_t f = i;
        while (i < ni && idx[i] < e) i++;
        agg_dense_run_add(t, agg_run_value(v, r), t->ibase + idx[f], i - f);
    }
    t->ii = i;
}
static ray_t* agg_dense_runs_bad(void) {
    return ray_error("corrupt", "group: a key run outside its dense plan");
}

/* Sel mode on runs: the task's selected rows [start, end) segment by
 * segment (ALL: a dense row range; MIX: the segment's indices). */
static __attribute__((noinline)) void agg_dense_runs_sel(agg_dense_ctx_t* c, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_local_t* loc = &c->locals[wid];
    if (loc->oom || end <= start) return;
    const agg_rr_t* rr = c->rr;
    ray_col_src_t* src = rr->srcs[0];
    if (ray_scan_fail_skip(rr->fail, start)) return;
    int64_t rows[AGG_SEL_CHUNK];
    uint32_t gid[AGG_SEL_CHUNK];
    agg_sel_scratch_t sc;
    if (!agg_sel_scratch_init(&sc, c->n_aggs)) { loc->oom = 1; return; }
    agg_curs_t cs; cs.cur = NULL;
    if (!agg_curs_init(&cs, rr)) { loc->oom = 1; agg_sel_scratch_free(&sc); return; }
    agg_dense_sel_run_t pr = { c, loc, rows, gid, &sc };
    agg_dense_runs_t t = { .c = c, .loc = loc };
    ray_enc_vview_t view;                               /* the task's key view: on its stack */
    int64_t vlo = -1, vhi = -1, nb = 0;                 /* the view's (or declined) rows; buffered rows */
    int vr = 0;
    bool stop = false;                                  /* cancelled */
    ray_t* err = NULL;
    const ray_rowsel_t* m = ray_rowsel_meta(c->sel);
    const uint8_t* flags = ray_rowsel_flags(c->sel);
    const uint32_t* offs = ray_rowsel_offsets(c->sel);
    const uint16_t* idx = ray_rowsel_idx(c->sel);
    const int64_t* pre = c->sel_prefix;
    uint32_t s = 0, hs = m->n_segs;                     /* the segment holding selected row `start` */
    while (s < hs) { uint32_t mid = s + (hs - s) / 2; if (pre[mid + 1] <= start) s = mid + 1; else hs = mid; }
    for (; s < m->n_segs && pre[s] < end && !err && !stop && !loc->oom; s++) {
        if (flags[s] == RAY_SEL_NONE) continue;
        const int64_t seg_sel = pre[s + 1] - pre[s];
        const int64_t p0 = start > pre[s] ? start - pre[s] : 0;
        const int64_t p1 = end - pre[s] < seg_sel ? end - pre[s] : seg_sel;
        const int64_t base = (int64_t)s * RAY_MORSEL_ELEMS;
        const bool all = flags[s] == RAY_SEL_ALL;
        const uint16_t* si = idx + offs[s];
        for (int64_t q = p0; q < p1 && !err; ) {
            const int64_t r = all ? base + q : base + si[q];
            if (r < vlo || r >= vhi) {
                if ((stop = agg_cancelled())) break;
                vr = ray_col_src_runview(src, r, &view, &vlo, &vhi, &err);
                if (vr < 0) break;
            }
            /* the positions [q, qe) of this segment inside [vlo, vhi) */
            int64_t qe;
            if (all) qe = vhi - base < p1 ? vhi - base : p1;
            else { qe = q + 1; while (qe < p1 && base + si[qe] < vhi) qe++; }
            if (vr > 0) {
                t.vlo = vlo;
                if (all) { t.idx = NULL; t.r0 = base + q; t.r1 = base + qe; }
                else { t.idx = si; t.ibase = base; t.ii = q; t.ni = qe; }
                if (all) (void)ray_enc_runs(&view, &t, agg_dense_run_rows);
                else agg_dense_runs_idx(&t, &view);
                if (t.bad) { err = agg_dense_runs_bad(); break; }
            } else {
                for (int64_t i = q; i < qe && !err; i++) {
                    rows[nb++] = all ? base + i : base + si[i];
                    if (nb == AGG_SEL_CHUNK) { err = agg_idx_walk(rr, &cs, rows, 0, nb, &pr, agg_dense_sel_rows, wid); nb = 0; }
                }
            }
            q = qe;
        }
    }
    if (nb > 0) {                                       /* the lower rows' read reports first */
        ray_t* ferr = loc->oom ? NULL : agg_idx_walk(rr, &cs, rows, 0, nb, &pr, agg_dense_sel_rows, wid);
        if (ferr) { if (err) ray_error_free(err); err = ferr; }
    }
    if (err) ray_scan_fail_note(rr->fail, start, err);
    agg_curs_free(&cs, rr);
    agg_sel_scratch_free(&sc);
}

/* Row mode on runs: rows [begin, limit), a key run view at a time; a
 * declined vector (or plain segment) is walked by the per-row body. */
static __attribute__((noinline)) void agg_dense_runs_rows(agg_dense_ctx_t* c, uint32_t task, int64_t begin, int64_t limit) {
    const agg_rr_t* rr = c->rr;
    if (ray_scan_fail_skip(rr->fail, begin)) return;
    agg_curs_t cs;
    if (!agg_curs_init(&cs, rr)) { ray_scan_fail_note(rr->fail, begin, ray_error("oom", NULL)); return; }
    agg_dense_runs_t t = { .c = c, .loc = &c->locals[task] };
    ray_enc_vview_t view;                               /* the task's key view: on its stack */
    for (int64_t r = begin; r < limit && !agg_cancelled() && !c->locals[task].oom; ) {
        int64_t vlo, vhi;
        ray_t* err = NULL;
        int vr = ray_col_src_runview(rr->srcs[0], r, &view, &vlo, &vhi, &err);
        if (vr < 0) { ray_scan_fail_note(rr->fail, begin, err); break; }
        const int64_t re = vhi < limit ? vhi : limit;
        if (vr > 0) {
            t.vlo = vlo; t.idx = NULL; t.r0 = r; t.r1 = re;
            (void)ray_enc_runs(&view, &t, agg_dense_run_rows);
            if (t.bad) { ray_scan_fail_note(rr->fail, begin, agg_dense_runs_bad()); break; }
        } else {
            bool ok = true;
            for (int64_t b = r; b < re && ok; b += 8192)
                ok = agg_walk_cs(rr, &cs, c, agg_dense_phaseA_fn, task, b, re - b < 8192 ? re : b + 8192, begin);
            if (!ok) break;
        }
        r = re;
    }
    agg_curs_free(&cs, rr);
}

/* One slab per logical task, independent of the physical worker executing it.
 * A large pool can run a memory-bounded number of dense tasks safely.  Each
 * 8192-row sub-block runs over the whole columns, or by pieces when a source
 * is staged (whatever the block's alignment, through the task's cursors; a
 * range error stops the task). */
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
        if (c->sel) agg_dense_phaseA_sel(c, (uint32_t)task, begin, limit);
        else if (!c->rr->staged)
            for (int64_t row = begin; row < limit && !agg_cancelled(); row += 8192) {
                ray_pool_scan_at(c->task_rows, row, limit);
                agg_dense_phaseA_fn(c, c->whole, (uint32_t)task, row, limit - row < 8192 ? limit : row + 8192);
            }
        else {
            agg_curs_t cs;
            if (!agg_curs_init(&cs, c->rr)) ray_scan_fail_note(c->rr->fail, begin, ray_error("oom", NULL));
            else {
                for (int64_t row = begin; row < limit && !agg_cancelled(); row += 8192)
                    if (!agg_walk_cs(c->rr, &cs, c, agg_dense_phaseA_fn, (uint32_t)task, row,
                                     limit - row < 8192 ? limit : row + 8192, begin)) break;
                agg_curs_free(&cs, c->rr);
            }
        }
        if (loc->eager) for (int64_t s = 0; s < loc->slots; s++)
            if (loc->first_row[s] != INT64_MAX)
                loc->occupied[s / 64] |= UINT64_C(1) << (s % 64);
    }
}

/* agg_dense_task_fn for ctx->runs (the slab's set-up and occupancy as
 * there), its own instance so the plain one keeps its code. */
static void agg_dense_task_runs_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_ctx_t* c = raw;
    int64_t chunk = c->task_rows / c->n_tasks;
    for (int64_t task = start; task < end; task++) {
        agg_dense_local_t* loc = &c->locals[task];
        agg_dense_local_init(loc, loc->slots, c->block);
        if (loc->eager) for (int64_t s = 0; s < loc->slots; s++) {
            loc->first_row[s] = INT64_MAX;
            for (uint32_t a = 0; a < c->n_aggs; a++)
                c->vts[a]->init(loc->states + (size_t)s * c->block + c->off[a]);
        }
        int64_t begin = chunk * task;
        int64_t limit = task + 1 == c->n_tasks ? c->task_rows : chunk * (task + 1);
        if (c->sel) agg_dense_runs_sel(c, (uint32_t)task, begin, limit);
        else agg_dense_runs_rows(c, (uint32_t)task, begin, limit);
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
        /* a cancelled dispatch skips tasks: their values and candidates
         * were never written */
        if (agg_cancelled()) atomic_store_explicit(&c.fail, 1, memory_order_relaxed);
        if (!atomic_load_explicit(&c.fail, memory_order_relaxed)) {
            if (cap > 0) {
                int64_t nc = 0;
                for (uint32_t t = 0; t < tasks; t++) {
                    memmove(cand + nc, cand + (size_t)t * cap, (size_t)cand_n[t] * sizeof(double));
                    nc += cand_n[t];
                }
                c.have_thr = agg_topn_union_threshold(cand, nc, n, ef, &c.thr);
            }
            if (tasks > 1) ray_pool_dispatch_n(pool, agg_slots_mark_fn, &c, tasks);
            else agg_slots_mark_fn(&c, 0, 0, 1);
            if (!agg_cancelled()) {
                kept = 0;
                for (uint32_t t = 0; t < tasks; t++) kept += c.kept[t];
            }
        }
    }
    ray_free_raw(vals); ray_free_raw(cand); ray_free_raw(cand_n);
    return kept;
}

/* Lean top-N emit over a shared dense slab.  The slab's occupancy bitmap is
 * walked in disjoint word ranges, one task each, in chunks of LEAN_CHUNK
 * slots: a chunk's filter-aggregate values feed a bounded candidate set per
 * task, the union of the sets gives the N-th value (the threshold), and a
 * second walk keeps the slots that pass agg_topn_mark.  Only the kept slots
 * are stored, so the emit allocates O(N) (plus ties) instead of one entry per
 * group; the kept set and its slot order are those of the generic path
 * (agg_slots_topn_select over every occupied slot).  The filter aggregate is
 * read from the states, or from a count-only slab of u32 counters. */
enum { LEAN_CHUNK = 4096, LEAN_CAND_BUDGET = 8 << 20 };
typedef struct {
    const agg_vtable_t* vt;
    const char* states;
    size_t block, off;
    int64_t param;
    const uint32_t* cnt32;
} agg_lean_src_t;

typedef struct {
    const agg_lean_src_t* src;
    const uint64_t* occ;
    int64_t words;
    const ray_group_emit_filter_t* ef;
    uint32_t tasks;
    int pass;           /* 0: candidates and group counts; 1: the kept slots */
    int64_t cap;
    double* cand;       /* [tasks][3 * cap]: the task's pool (2 * cap), then scratch */
    int64_t* pool_n;
    int64_t* groups;
    bool have_thr;
    double thr;
    int64_t** kept;
    int64_t* kept_n;
    int64_t* kept_cap;
    _Atomic(int) fail;
} agg_lean_ctx_t;

static bool agg_lean_vals(const agg_lean_src_t* s, const int64_t* slots, int64_t n, double* out) {
    if (s->cnt32) {
        for (int64_t i = 0; i < n; i++) out[i] = (double)s->cnt32[slots[i]];
        return true;
    }
    return agg_group_values_f64(s->vt, s->states, s->block, s->off, slots, n, s->param, out);
}

/* The next chunk of occupied slots of words [*w, w1); *bits holds the rest of
 * word *w (consumed bits cleared). */
static int64_t agg_lean_chunk(const uint64_t* occ, int64_t* w, uint64_t* bits, int64_t w1, int64_t* slots) {
    int64_t n = 0;
    for (;;) {
        while (*bits) {
            if (n == LEAN_CHUNK) return n;
            slots[n++] = *w * 64 + __builtin_ctzll(*bits);
            *bits &= *bits - 1;
        }
        if (++*w >= w1) return n;
        *bits = occ[*w];
    }
}

static void agg_lean_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_lean_ctx_t* c = raw;
    const ray_group_emit_filter_t* ef = c->ef;
    int64_t slots[LEAN_CHUNK];
    double vals[LEAN_CHUNK];
    uint8_t keep[LEAN_CHUNK];
    for (int64_t t = start; t < end; t++) {
        int64_t w0 = c->words / c->tasks * t;
        int64_t w1 = t + 1 == (int64_t)c->tasks ? c->words : c->words / c->tasks * (t + 1);
        if (w0 >= w1) continue;
        int64_t w = w0;
        uint64_t bits = c->occ[w];
        for (;;) {
            int64_t n = agg_lean_chunk(c->occ, &w, &bits, w1, slots);
            if (n == 0) break;
            if (!agg_lean_vals(c->src, slots, n, vals)) {
                atomic_store_explicit(&c->fail, 1, memory_order_relaxed);
                return;
            }
            if (c->pass == 0) {
                c->groups[t] += n;
                if (c->cap > 0) {
                    double* pool = c->cand + (size_t)t * 3 * c->cap;
                    double* tmp = pool + 2 * c->cap;
                    int64_t hn;
                    agg_topn_candidates(vals, n, c->cap, ef->desc, tmp, &hn);
                    memcpy(pool + c->pool_n[t], tmp, (size_t)hn * sizeof(double));
                    c->pool_n[t] += hn;
                    if (c->pool_n[t] > c->cap) {
                        agg_topn_candidates(pool, c->pool_n[t], c->cap, ef->desc, tmp, &hn);
                        memcpy(pool, tmp, (size_t)hn * sizeof(double));
                        c->pool_n[t] = hn;
                    }
                }
            } else {
                int64_t k = agg_topn_mark(vals, n, ef, c->have_thr, c->thr, keep);
                if (c->kept_n[t] + k > c->kept_cap[t]) {
                    int64_t nc = c->kept_cap[t] ? c->kept_cap[t] * 2 : 64;
                    while (nc < c->kept_n[t] + k) nc *= 2;
                    int64_t* p = ray_realloc_raw(c->kept[t], (size_t)nc * sizeof(int64_t));
                    if (!p) { atomic_store_explicit(&c->fail, 1, memory_order_relaxed); return; }
                    c->kept[t] = p; c->kept_cap[t] = nc;
                }
                for (int64_t i = 0; i < n; i++)
                    if (keep[i]) c->kept[t][c->kept_n[t]++] = slots[i];
            }
        }
    }
}

/* The kept slots among the occupied ones, ascending, in *out (the caller
 * frees it); their count, or -1 when the filter aggregate has no scalar order
 * or memory ran out. */
static int64_t agg_dense_lean_select(ray_pool_t* pool, const agg_lean_src_t* src,
        const uint64_t* occ, int64_t total_slots, const ray_group_emit_filter_t* ef,
        int64_t** out, bool* have_thr, double* thr) {
    *out = NULL; *have_thr = false; *thr = 0;
    int64_t words = (total_slots + 63) / 64;
    int64_t cap = ef->top_count_take > 0 ? ef->top_count_take : 0;
    uint32_t tasks = pool && total_slots >= RAY_PARALLEL_THRESHOLD ? ray_pool_total_workers(pool) * 4 : 1;
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    if ((int64_t)tasks > words) tasks = (uint32_t)words;
    if (cap > 0 && (int64_t)tasks * 3 * cap > LEAN_CAND_BUDGET) {
        int64_t fit = LEAN_CAND_BUDGET / (3 * cap);
        if (fit < tasks) tasks = fit < 1 ? 1 : (uint32_t)fit;
    }
    if (tasks < 1) tasks = 1;
    size_t per = cap > 0 ? (size_t)cap : 1;
    double* cand = ray_alloc_raw((size_t)tasks * 3 * per * sizeof(double));
    double* all = ray_alloc_raw((size_t)tasks * per * sizeof(double));
    int64_t* meta = ray_calloc_raw((size_t)tasks * 4 * sizeof(int64_t));
    int64_t** kept = ray_calloc_raw((size_t)tasks * sizeof(int64_t*));
    int64_t result = -1;
    if (cand && all && meta && kept) {
        agg_lean_ctx_t c = { .src = src, .occ = occ, .words = words, .ef = ef, .tasks = tasks,
            .cap = cap, .cand = cand, .pool_n = meta, .groups = meta + tasks,
            .kept = kept, .kept_n = meta + 2 * tasks, .kept_cap = meta + 3 * tasks };
        atomic_init(&c.fail, 0);
        if (tasks > 1) ray_pool_dispatch_n(pool, agg_lean_fn, &c, tasks);
        else agg_lean_fn(&c, 0, 0, 1);
        /* a cancelled dispatch skips tasks: their counts and candidates
         * never got written, the selection is refused like a failed one */
        if (agg_cancelled()) atomic_store_explicit(&c.fail, 1, memory_order_relaxed);
        if (!atomic_load_explicit(&c.fail, memory_order_relaxed)) {
            int64_t total = 0, nc = 0;
            for (uint32_t t = 0; t < tasks; t++) {
                total += c.groups[t];
                memcpy(all + nc, cand + (size_t)t * 3 * per, (size_t)c.pool_n[t] * sizeof(double));
                nc += c.pool_n[t];
            }
            if (cap > 0) c.have_thr = agg_topn_union_threshold(all, nc, total, ef, &c.thr);
            *have_thr = c.have_thr; *thr = c.thr;
            c.pass = 1;
            if (tasks > 1) ray_pool_dispatch_n(pool, agg_lean_fn, &c, tasks);
            else agg_lean_fn(&c, 0, 0, 1);
            if (agg_cancelled()) atomic_store_explicit(&c.fail, 1, memory_order_relaxed);
            if (!atomic_load_explicit(&c.fail, memory_order_relaxed)) {
                int64_t n = 0;
                for (uint32_t t = 0; t < tasks; t++) n += c.kept_n[t];
                int64_t* o = ray_alloc_raw((size_t)(n > 0 ? n : 1) * sizeof(int64_t));
                if (o) {
                    int64_t at = 0;
                    for (uint32_t t = 0; t < tasks; t++) {
                        if (c.kept_n[t]) memcpy(o + at, c.kept[t], (size_t)c.kept_n[t] * sizeof(int64_t));
                        at += c.kept_n[t];
                    }
                    *out = o;
                    result = n;
                }
            }
        }
        for (uint32_t t = 0; t < tasks; t++) ray_free_raw(kept[t]);
    }
    ray_free_raw(cand); ray_free_raw(all); ray_free_raw(meta); ray_free_raw(kept);
    return result;
}

/* The occupancy bitmap of a first-row array: bit s is set when slot s holds a
 * group (first[s] != INT64_MAX).  Word ranges, one task each. */
typedef struct {
    const int64_t* first;
    uint64_t*      occ;
    int64_t        slots, words;
    uint32_t       tasks;
} agg_occ_first_ctx_t;

static void agg_occ_first_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_occ_first_ctx_t* c = raw;
    for (int64_t t = start; t < end; t++) {
        int64_t w0 = c->words / c->tasks * t;
        int64_t w1 = t + 1 == (int64_t)c->tasks ? c->words : c->words / c->tasks * (t + 1);
        for (int64_t w = w0; w < w1; w++) {
            int64_t base = w * 64;
            int64_t n = c->slots - base < 64 ? c->slots - base : 64;
            uint64_t b = 0;
            for (int64_t i = 0; i < n; i++) b |= (uint64_t)(c->first[base + i] != INT64_MAX) << i;
            c->occ[w] = b;
        }
    }
}

/* The occupied slots of a bitmap in ascending order: tasks count the set bits
 * of their word ranges, a prefix places them, and each task writes its
 * range's slots at its place.  1 when out holds them, 0 when the counts do
 * not add up to ng (the caller collects serially), -1 when the query was
 * cancelled under a dispatch: the pool skips the tickets of a cancelled
 * query, so the counts, or out, were never written. */
typedef struct {
    const uint64_t* occ;
    int64_t*        out;
    int64_t*        at;        /* [tasks + 1]: counts, then first positions */
    int64_t         words;
    uint32_t        tasks;
    int             pass;      /* 0: count, 1: write */
} agg_occ_collect_ctx_t;

static void agg_occ_collect_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_occ_collect_ctx_t* c = raw;
    for (int64_t t = start; t < end; t++) {
        int64_t w0 = c->words / c->tasks * t;
        int64_t w1 = t + 1 == (int64_t)c->tasks ? c->words : c->words / c->tasks * (t + 1);
        if (c->pass == 0) {
            int64_t n = 0;
            for (int64_t w = w0; w < w1; w++) n += __builtin_popcountll(c->occ[w]);
            c->at[t] = n;
        } else {
            int64_t j = c->at[t];
            for (int64_t w = w0; w < w1; w++)
                for (uint64_t b = c->occ[w]; b; b &= b - 1) c->out[j++] = w * 64 + __builtin_ctzll(b);
        }
    }
}

static int agg_occ_collect(ray_pool_t* pool, const uint64_t* occ, int64_t slots, int64_t ng, int64_t* out) {
    int64_t words = (slots + 63) / 64;
    uint32_t tasks = ray_pool_par_dispatch_ok(pool, slots, RAY_PARALLEL_THRESHOLD)
                   ? ray_pool_total_workers(pool) * 4 : 1;
    if ((int64_t)tasks > words) tasks = (uint32_t)(words > 0 ? words : 1);
    if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
    int64_t* at = ray_alloc_raw(((size_t)tasks + 1) * sizeof(int64_t));
    if (!at) return 0;
    agg_occ_collect_ctx_t c = { .occ = occ, .out = out, .at = at, .words = words, .tasks = tasks, .pass = 0 };
    if (tasks > 1) ray_pool_dispatch_n(pool, agg_occ_collect_fn, &c, tasks);
    else agg_occ_collect_fn(&c, 0, 0, 1);
    if (agg_cancelled()) { ray_free_raw(at); return -1; }
    int64_t run = 0;
    for (uint32_t t = 0; t < tasks; t++) { int64_t n = at[t]; at[t] = run; run += n; }
    int ok = run == ng;
    if (ok) {
        c.pass = 1;
        if (tasks > 1) ray_pool_dispatch_n(pool, agg_occ_collect_fn, &c, tasks);
        else agg_occ_collect_fn(&c, 0, 0, 1);
        if (agg_cancelled()) ok = -1;
    }
    ray_free_raw(at);
    return ok;
}

/* Shared output stage. Takes ownership of states/first/occ (occ: a bitmap of
 * the occupied slots that stands in for first; first is then NULL), borrows the
 * prepared aggregate layout and descriptors.  u32c: gstates is a count-only slab
 * of u32 counters, one per slot, standing for every (COUNT) aggregate. */
static ray_t* agg_dense_finish(ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext,
        ray_pool_t* pool, const agg_vo_t* vo, const agg_desc_t* d,
        int64_t total_slots, char* gstates, int64_t* gfirst, const uint64_t* occ,
        const dense_plan_t* key_plan, int64_t key_part_slots, uint32_t key_bits,
        const ray_group_emit_filter_t* ef, int64_t group_limit, const dense_plan_t* slot_plan,
        bool u32c, int64_t* pre_slots, int64_t pre_n) {
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
    int64_t* occupied_slot = NULL;
    int64_t* first_row_ordered = NULL;
    /* A first-row array of a key-planned output (the keys are rebuilt from
     * the slots, the array only says which are occupied): its occupancy as
     * a bitmap, so the selection and the collection below walk the words on
     * the workers instead of the slots in one thread. */
    if (!occ && gfirst && key_plan && group_limit <= 0 && total_slots > 0) {
        int64_t words = (total_slots + 63) / 64;
        uint64_t* ob = ray_alloc_raw((size_t)words * sizeof(uint64_t));
        if (ob) {
            uint32_t tasks = ray_pool_par_dispatch_ok(pool, total_slots, RAY_PARALLEL_THRESHOLD)
                           ? ray_pool_total_workers(pool) * 4 : 1;
            if ((int64_t)tasks > words) tasks = (uint32_t)words;
            if (tasks > RAY_POOL_INIT_TASKS) tasks = RAY_POOL_INIT_TASKS;
            agg_occ_first_ctx_t oc = { .first = gfirst, .occ = ob, .slots = total_slots,
                                       .words = words, .tasks = tasks };
            if (tasks > 1) ray_pool_dispatch_n(pool, agg_occ_first_fn, &oc, tasks);
            else agg_occ_first_fn(&oc, 0, 0, 1);
            /* the tickets of a cancelled query are skipped: ob's words
             * were never written */
            if (agg_cancelled()) {
                ray_free_raw(ob);
                agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
                ray_free_raw(gstates); ray_free_raw(gfirst);
                return ray_error("cancel", NULL);
            }
            ray_free_raw(gfirst);
            gfirst = NULL;
            occ = ob;
        }
    }
    /* A slab with an occupancy bitmap under a top-N filter keeps only the
     * groups the filter keeps (agg_dense_lean_select): no entry per group. */
    if (occ && ef && key_plan && group_limit <= 0) {
        agg_lean_src_t src = { .vt = vts[ef->agg_index], .states = gstates, .block = block,
            .off = off[ef->agg_index], .param = ext->agg_k ? ext->agg_k[ef->agg_index] : 0,
            .cnt32 = u32c ? (const uint32_t*)gstates : NULL };
        bool lean_thr; double lean_thrv;
        int64_t kept = pre_n >= 0 ? pre_n : agg_dense_lean_select(pool, &src, occ, total_slots, ef, &occupied_slot, &lean_thr, &lean_thrv);
        if (pre_n >= 0) occupied_slot = pre_slots;
        if (kept >= 0) {
            ng = kept;
            route_stats.topn_native = true;
            route_stats.topn_kept = kept;
            route_stats.dense_emit_slots = (uint64_t)kept;
            ray_profile_tick("dense: selected top-N groups from the slab");
            goto emit;
        }
        if (u32c) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
            return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
        }
    }
    if (occ) for (int64_t w = 0; w < (total_slots + 63) / 64; w++) ng += __builtin_popcountll(occ[w]);
    else for (int64_t s = 0; s < total_slots; s++) if (gfirst[s] != INT64_MAX) ng++;
    route_stats.dense_emit_slots = (uint64_t)ng;
    ray_profile_tick("dense: freed slabs and counted groups");

    occupied_slot     = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    first_row_ordered = key_plan ? NULL : ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    if (!occupied_slot || (!key_plan && !first_row_ordered)) {
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered);
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
        return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    int collected = occ && !first_row_ordered ? agg_occ_collect(pool, occ, total_slots, ng, occupied_slot) : 0;
    if (collected < 0) {
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered);
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
        return ray_error("cancel", NULL);
    }
    if (!collected) {
      int64_t i = 0;
      for (int64_t s = 0; s < total_slots; s++)
          if (occ ? (occ[s >> 6] >> (s & 63)) & 1 : gfirst[s] != INT64_MAX) {
              if (first_row_ordered) first_row_ordered[i] = gfirst[s];
              occupied_slot[i] = s; i++;
          }
    }
    /* Bounded emit (HEAD(GROUP) hint): the N groups with the smallest first
     * row ARE the first N groups in first-seen order.  Select them with an
     * N-sized max-heap over the occupied slots and emit them ascending by
     * first row, byte-identical to trimming a first-seen-ordered result. */
    if (group_limit > 0 && first_row_ordered && ng > 0) {
        int64_t n_keep = ng < group_limit ? ng : group_limit;
        int64_t* hkey = ray_alloc_raw((size_t)n_keep * sizeof(int64_t));
        int64_t* hslot = ray_alloc_raw((size_t)n_keep * sizeof(int64_t));
        if (!hkey || !hslot) {
            /* Emitting slot order here would hand the caller's head the wrong
             * prefix; fail the query instead of answering it differently. */
            ray_free_raw(hkey); ray_free_raw(hslot);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered);
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
            return ray_error("oom", NULL);
        }
        {
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
            route_stats.topn_kept = kept;
        }
        ray_free_raw(keep);
        ray_profile_tick("dense: selected top-N groups");
    }

emit:
    ;
    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
        return result ? result : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }

    for (uint32_t k = 0; k < n_keys; k++) {
        /* A staged key is never gathered at first rows (random reads): its
         * value is rebuilt from the group's slot, the inverse of
         * agg_dense_slot (agg_dense_code: null slot, compaction). */
        const dense_plan_t* kp = key_plan ? key_plan : d->key_srcs[k] ? slot_plan : NULL;
        ray_t* kc = kp ? col_vec_new(key_cols[k], ng)
                       : ray_group_gather(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
            ray_release(result); return kc ? kc : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
        }
        if (kp) {
            kc->len = ng;
            if (ray_vec_may_have_nulls(key_cols[k])) kc->attrs |= RAY_ATTR_HAS_NULLS;
            if (kc->type == RAY_SYM) ray_sym_vec_adopt_domain(kc, sym_domain_rep(key_cols[k]));
            agg_dense_key_emit_t emit = {kp, occupied_slot, key_plan ? key_part_slots : 0,
                                         key_plan ? key_bits : 0, kc, k};
            ray_pool_dispatch(pool, agg_dense_key_emit, &emit, ng);
            ray_group_key_settle_nulls(kc);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint32_t a = 0; a < n_aggs; a++) {
        if (u32c) {   /* the slab's counters are the count column */
            ray_t* cnt_out = ray_vec_new(RAY_I64, ng);
            if (!cnt_out || RAY_IS_ERR(cnt_out)) {
                ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
                ray_release(result); return cnt_out ? cnt_out : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
            }
            cnt_out->len = ng;
            const uint32_t* cnt = (const uint32_t*)gstates;
            int64_t* dst = (int64_t*)ray_data(cnt_out);
            for (int64_t i = 0; i < ng; i++) dst[i] = cnt[occupied_slot[i]];
            result = ray_table_add_col(result, agg_result_col_name(agg_syms[a], ext->agg_ops[a]), cnt_out);
            ray_release(cnt_out);
            continue;
        }
        /* Buffered top_n/bot_n produce a LIST cell per group (a native vector);
         * median and all streaming aggs produce a scalar out_type cell. */
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = is_list ? ray_list_new(ng)
                             : ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
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
    ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst); ray_free_raw((void*)occ);
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
    int64_t in_rows;               /* rows of the input selected_rows index */
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
    const agg_rr_t* rr;          /* sources; staged ones are read by range */
    const agg_view_t* whole;
    /* The staged sources each pass reads (rr's otherwise): the histogram
     * the keys, the scatter the inputs, so each decodes once. */
    const agg_rr_t* count_rr;
    const agg_rr_t* scatter_rr;
} agg_dense_partition_ctx_t;

/* One source task of the histogram / scatter passes: its count row, and
 * its write cursors (persisting across the pieces of the task). */
typedef struct {
    agg_dense_partition_ctx_t* c;
    uint64_t* count;
    uint64_t* cursor;
} agg_part_task_t;

/* Run body over input positions [begin, limit) of a source task: the
 * whole columns, pieces of the rows (R-WALK), or runs of the selected rows
 * (R-SPARSE through the task's cursors). */
static void agg_dense_partition_run(agg_dense_partition_ctx_t* c, const agg_rr_t* rr, agg_part_task_t* t,
                                    agg_body_fn body, uint32_t wid, int64_t begin, int64_t limit) {
    if (!rr->staged) {
        if (begin >= limit) return;
        /* These passes are the first reads of the keys and the aggregate
         * inputs: keep the rows past each block requested from storage
         * (scan read-ahead), from the input row the block starts at —
         * selected rows come in order. */
        const int64_t* sel = c->selected_rows;
        int64_t stop = sel ? sel[limit - 1] + 1 : limit;
        for (int64_t b = begin; b < limit; b += 8192) {
            ray_pool_scan_at(c->in_rows, sel ? sel[b] : b, stop);
            body(t, c->whole, wid, b, limit - b > 8192 ? b + 8192 : limit);
        }
        return;
    }
    if (!c->selected_rows) { (void)agg_walk(rr, t, body, wid, begin, limit, begin); return; }
    if (ray_scan_fail_skip(rr->fail, begin)) return;
    agg_curs_t cs;
    if (!agg_curs_init(&cs, rr)) { ray_scan_fail_note(rr->fail, begin, ray_error("oom", NULL)); return; }
    ray_t* err = agg_idx_walk(rr, &cs, c->selected_rows, begin, limit, t, body, wid);
    agg_curs_free(&cs, rr);
    if (err) ray_scan_fail_note(rr->fail, begin, err);
}

static void agg_dense_partition_count_rows(void* raw, const agg_view_t* v, uint32_t wid,
                                           int64_t begin, int64_t limit) {
    (void)wid;
    agg_part_task_t* t = raw;
    agg_dense_partition_ctx_t* c = t->c;
    uint64_t* count = t->count;
    const int64_t kbase = v->base;
    const void* data = v->key_data[0];
    if (c->plan->n_keys > 1) {
        #define PART_SLOT_MULTI(COMPONENT)                                 \
            for (int64_t r = begin; r < limit; r++) {                    \
                int64_t source = (c->selected_rows ? c->selected_rows[r] : r) - kbase; \
                int64_t slot = 0;                                        \
                for (uint32_t k = 0; k < c->plan->n_keys; k++)           \
                    slot += COMPONENT(c->plan, k, agg_read_key_i64_raw(c->keys[k]->type, v->key_attrs[k], v->key_data[k], source)) * c->plan->strides[k]; \
                c->gids[r] = (uint32_t)slot;                             \
                count[slot & (c->parts - 1)]++;                          \
            }
        if (c->plan->compacted) { PART_SLOT_MULTI(agg_dense_component); }
        else { PART_SLOT_MULTI(agg_dense_component_raw); }
        #undef PART_SLOT_MULTI
        return;
    }
    #define PART_COUNT(T) do { \
        const T* p = data; \
        for (int64_t r = begin; r < limit; r++) { \
            uint32_t slot = (uint32_t)agg_dense_component_raw(c->plan, 0, (int64_t)p[(c->selected_rows ? c->selected_rows[r] : r) - kbase]); \
            c->gids[r] = slot; \
            count[slot & (c->parts - 1)]++; \
        } \
    } while (0)
    switch (c->key->type) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: PART_COUNT(int64_t); break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: PART_COUNT(int32_t); break;
        case RAY_I16: PART_COUNT(int16_t); break;
        case RAY_U8: case RAY_BOOL: PART_COUNT(uint8_t); break;
        case RAY_SYM:
            switch (v->key_attrs[0] & RAY_SYM_W_MASK) {
                case RAY_SYM_W8: PART_COUNT(uint8_t); break;
                case RAY_SYM_W16: PART_COUNT(uint16_t); break;
                case RAY_SYM_W32: PART_COUNT(uint32_t); break;
                default: PART_COUNT(int64_t); break;
            }
            break;
    }
    #undef PART_COUNT
}

static void agg_dense_partition_count(void* raw, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_partition_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->sources * task;
        int64_t limit = task + 1 == c->sources ? c->rows : c->rows / c->sources * (task + 1);
        agg_part_task_t t = { c, c->counts + task * c->parts, NULL };
        agg_dense_partition_run(c, c->count_rr, &t, agg_dense_partition_count_rows, wid, begin, limit);
    }
}

/* Resolve native widths outside the row loop. A runtime-sized memcpy per
 * value dominated scatter for eight-byte inputs and mixed aggregates.
 * `data` is the view's pointer at row `base`. */
static void agg_dense_partition_field(agg_dense_partition_ctx_t* c, const void* data, int64_t base,
        uint8_t width, size_t field, const uint64_t* starts, int64_t begin, int64_t end) {
    uint64_t cursor[RAY_POOL_INIT_TASKS / 2];
    assert(c->parts <= RAY_POOL_INIT_TASKS / 2);
    memcpy(cursor, starts, c->parts * sizeof(*cursor));
    #define SCATTER_FIELD(W) do { \
        for (int64_t r = begin; r < end; r++) { \
            uint32_t part = c->gids[r] & (c->parts - 1); \
            memcpy(c->payload + cursor[part]++ * c->record_size + field, \
                   (const char*)data + (size_t)((c->selected_rows ? c->selected_rows[r] : r) - base) * (W), (W)); \
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

static void agg_dense_partition_scatter_rows(void* raw, const agg_view_t* v, uint32_t wid,
                                             int64_t begin, int64_t limit) {
    (void)wid;
    agg_part_task_t* t = raw;
    agg_dense_partition_ctx_t* c = t->c;
    uint64_t* cursor = t->cursor;
    const int64_t base = v->base;
    const agg_valdesc_t* vd = &c->values;
    if (vd->n_aggs == 1 && vd->val_data[0] && !vd->val2_data[0]) {
        const char* vals = v->val_data[0];
        #define SCATTER_UNARY(W) do { \
            for (int64_t r = begin; r < limit; r++) { \
                uint32_t slot = c->gids[r]; \
                uint32_t gid = slot >> c->bits, part = slot & (c->parts - 1); \
                char* dst = c->payload + cursor[part]++ * (4 + (W)); \
                memcpy(dst, &gid, 4); \
                memcpy(dst + 4, vals + (size_t)((c->selected_rows ? c->selected_rows[r] : r) - base) * (W), (W)); \
            } \
        } while (0)
        switch (vd->val_esz[0]) {
            case 1: SCATTER_UNARY(1); break;
            case 2: SCATTER_UNARY(2); break;
            case 4: SCATTER_UNARY(4); break;
            case 8: SCATTER_UNARY(8); break;
        }
        #undef SCATTER_UNARY
        return;
    }
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
            if (v->val_data[a] && c->value_offsets[a] >= emitted) {
                agg_dense_partition_field(c, v->val_data[a], base, vd->val_esz[a], c->value_offsets[a], chunk_starts, chunk, chunk_end);
                emitted = c->value_offsets[a] + vd->val_esz[a];
            }
            if (v->val2_data[a] && c->value2_offsets[a] >= emitted) {
                agg_dense_partition_field(c, v->val2_data[a], base, vd->val2_esz[a], c->value2_offsets[a], chunk_starts, chunk, chunk_end);
                emitted = c->value2_offsets[a] + vd->val2_esz[a];
            }
        }
    }
}

static void agg_dense_partition_scatter(void* raw, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_partition_ctx_t* c = raw;
    for (int64_t task = start; task < end; task++) {
        int64_t begin = c->rows / c->sources * task;
        int64_t limit = task + 1 == c->sources ? c->rows : c->rows / c->sources * (task + 1);
        const uint64_t* starts = c->counts + task * c->parts;
        uint64_t cursor[RAY_POOL_INIT_TASKS / 2];
        assert(c->parts <= RAY_POOL_INIT_TASKS / 2);
        memcpy(cursor, starts, c->parts * sizeof(*cursor));
        agg_part_task_t t = { c, NULL, cursor };
        agg_dense_partition_run(c, c->scatter_rr, &t, agg_dense_partition_scatter_rows, wid, begin, limit);
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
    /* An input's identity: its plain pointer, or its staged source (one
     * source per column and table: ray_graph_bind_col deduplicates). */
    for (uint32_t f = 0; f < 2 * n; f++) {
        uint32_t a = f / 2;
        const void* data = f % 2 ? d->val2_id[a] : d->val_id[a];
        uint8_t width = f % 2 ? d->val2_esz[a] : d->val_esz[a];
        size_t offset = bytes;
        bool found = false;
        for (uint32_t prev = 0; data && prev < f; prev++) {
            uint32_t b = prev / 2;
            const void* other = prev % 2 ? d->val2_id[b] : d->val_id[b];
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
        const agg_vo_t* vo, agg_desc_t* d, ray_t* selection,
        const ray_group_emit_filter_t* ef) {
    ray_t* ferr = NULL;   /* R-FAIL: a range error of a staged source */
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
        .rr = &d->rr, .whole = &d->whole,
        .plan = plan, .rows = rows, .part_slots = part_slots,
        .in_rows = selection ? ray_rowsel_meta(selection)->nrows : rows,
        .sources = sources, .parts = parts, .bits = bits, .failed = false,
        .values = { .n_aggs = ext->n_aggs, .vts = vo->vts, .off = vo->off, .block = vo->block,
            .val_data = d->val_id, .val_types = d->val_types, .val_hasnull = d->val_hasnull, .val_esz = d->val_esz,
            .val2_data = d->val2_id, .val2_types = d->val2_types, .val2_hasnull = d->val2_hasnull, .val2_esz = d->val2_esz }
    };
    /* Per-pass source sets: a staged source leaves the pass that does not
     * read it (its slot then reads as an absent input). */
    ray_col_src_t** pass_srcs = NULL;
    agg_rr_t count_rr = d->rr, scatter_rr = d->rr;
    c.count_rr = c.scatter_rr = &d->rr;
    if (d->staged) {
        uint32_t ns = d->rr.n_src, nk = d->rr.n_keys;
        pass_srcs = ray_alloc_raw((size_t)2 * (ns ? ns : 1) * sizeof(*pass_srcs));
        if (!pass_srcs) goto failed;
        for (uint32_t i = 0; i < ns; i++) {
            pass_srcs[i] = i < nk ? d->rr.srcs[i] : NULL;
            pass_srcs[ns + i] = i < nk ? NULL : d->rr.srcs[i];
        }
        count_rr.srcs = pass_srcs;
        scatter_rr.srcs = pass_srcs + ns;
        c.count_rr = &count_rr;
        c.scatter_rr = &scatter_rr;
    }
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
    agg_desc_advise(d, selection ? ray_rowsel_meta(selection)->nrows : rows);
    ray_pool_dispatch_n(pool, agg_dense_partition_count, &c, sources);
    if (d->staged && (ferr = ray_scan_fail_finish(&d->fail, pool)) != NULL) goto failed;
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
    if (d->staged && (ferr = ray_scan_fail_finish(&d->fail, pool)) != NULL) goto failed;
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
    ray_free_raw(pass_srcs);
    route_stats.dense_strategy = AGG_DENSE_PARTITIONED;
    route_stats.dense_tasks = c.n_tasks;
    route_stats.dense_local_slots = slots + extra_slots;
    return agg_dense_finish(key_cols, key_syms, ext, pool, vo, d, slots, c.states, c.first, NULL, plan, part_slots, bits, ef, 0, plan, false, NULL, -1);
failed:
    ray_free_raw(c.counts); ray_free_raw(c.gids); ray_free_raw(c.tasks);
    ray_free_raw(c.payload); ray_free_raw(c.value_offsets); ray_release(c.selection_indices); ray_free_raw(c.states); ray_free_raw(c.first);
    ray_free_raw(pass_srcs);
    return ferr ? ferr : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
}

typedef struct {
    ray_t* key;
    const dense_plan_t* plan;
    const agg_vo_t* layout;
    const agg_desc_t* desc;
    uint32_t n_aggs;
    char* states;
    uint32_t* cnt32;                /* the count-only slab (agg_count32_slab), else NULL */
    int64_t slots;
    /* A probe pass (agg_dense_cut_ties) reads the keys again and records, for
     * each slot of pslots (ascending) whose bit is set in probe, the first row
     * that holds it in pfirst; it updates no counter. */
    const uint64_t* probe;
    const int64_t* pslots;
    int64_t pn;
    int64_t* pfirst;
    ray_t* sel;                     /* the selection (selected-row space), else NULL */
    const int64_t* sel_prefix;
    const agg_valdesc_t* vd;
    _Atomic(uint64_t)* occupied;
} agg_dense_shared_ctx_t;

static inline void agg_probe_note(const agg_dense_shared_ctx_t* c, uint32_t slot, int64_t row) {
    if (!((c->probe[slot >> 6] >> (slot & 63)) & 1)) return;
    int64_t lo = 0, hi = c->pn - 1;
    while (lo < hi) {
        int64_t mid = (lo + hi) / 2;
        if (c->pslots[mid] < (int64_t)slot) lo = mid + 1; else hi = mid;
    }
    int64_t seen = __atomic_load_n(&c->pfirst[lo], __ATOMIC_RELAXED);
    while (row < seen &&
           !__atomic_compare_exchange_n(&c->pfirst[lo], &seen, row, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

static void agg_dense_shared_zero(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    memset(c->cnt32 + start, 0, (size_t)(end - start) * sizeof(uint32_t));
}

/* The count-only slab keeps no bit per row: a slot is occupied when its
 * counter is not zero. */
static void agg_dense_shared_cnt_occupied(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    for (int64_t w = start; w < end; w++) {
        uint64_t m = 0;
        for (int64_t b = 0; b < 64 && w * 64 + b < c->slots; b++)
            if (c->cnt32[w * 64 + b]) m |= UINT64_C(1) << b;
        atomic_store_explicit(&c->occupied[w], m, memory_order_relaxed);
    }
}

static void agg_dense_shared_init(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    const agg_vo_t* vo = c->layout;
    for (int64_t s = start; s < end; s++)
        for (uint32_t a = 0; a < c->n_aggs; a++)
            vo->vts[a]->init(c->states + (size_t)s * vo->block + vo->off[a]);
}

/* Rows [start, end) (at most 8192) read through view v. */
static void agg_dense_shared_block(void* raw, const agg_view_t* v, uint32_t wid, int64_t begin, int64_t end) {
    (void)wid;
    agg_dense_shared_ctx_t* c = raw;
    const agg_vo_t* vo = c->layout;
    const agg_desc_t* d = c->desc;
    const void* data = v->key_data[0];
    const int64_t base = v->base;
    uint32_t gids[8192];
    int64_t n = end - begin;
    uint32_t* cnt32 = c->cnt32;
    const uint64_t* probe = c->probe;
    #define SHARED_GIDS(T) do { \
        const T* p = (const T*)data + (begin - base); \
        for (int64_t i = 0; i < n; i++) { \
            uint32_t slot = (uint32_t)agg_dense_component_raw(c->plan, 0, (int64_t)p[i]); \
            gids[i] = slot; \
            if (probe) { agg_probe_note(c, slot, begin + i); continue; } \
            if (cnt32) { __atomic_fetch_add(&cnt32[slot], 1, __ATOMIC_RELAXED); continue; } \
            uint64_t bit = UINT64_C(1) << (slot % 64); \
            _Atomic(uint64_t)* word = &c->occupied[slot / 64]; \
            if (!(atomic_load_explicit(word, memory_order_relaxed) & bit)) \
                atomic_fetch_or_explicit(word, bit, memory_order_relaxed); \
        } \
    } while (0)
    switch (c->key->type) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: SHARED_GIDS(int64_t); break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: SHARED_GIDS(int32_t); break;
        case RAY_I16: SHARED_GIDS(int16_t); break;
        case RAY_U8: case RAY_BOOL: SHARED_GIDS(uint8_t); break;
        case RAY_SYM:
            switch (v->key_attrs[0] & RAY_SYM_W_MASK) {
                case RAY_SYM_W8: SHARED_GIDS(uint8_t); break;
                case RAY_SYM_W16: SHARED_GIDS(uint16_t); break;
                case RAY_SYM_W32: SHARED_GIDS(uint32_t); break;
                default: SHARED_GIDS(int64_t); break;
            }
            break;
    }
    #undef SHARED_GIDS
    if (cnt32 || probe) return;
    for (uint32_t a = 0; a < c->n_aggs; a++) {
        const void* vals = v->val_data[a]   /* NULL: COUNT reads no input */
            ? (const char*)v->val_data[a] + (size_t)(begin - base) * d->val_esz[a] : NULL;
        ray_valid_t valid = {vals, d->val_types[a], d->val_hasnull[a]};
        vo->vts[a]->update_shared(c->states + vo->off[a], vo->block, gids, vals, &valid, n);
    }
}

/* A run of selected rows rows[ib, ie) read through view v: their slots, then
 * the counters or the shared kernels. */
typedef struct {
    agg_dense_shared_ctx_t* c;
    const int64_t* rows;
    uint32_t* gid;
    agg_sel_scratch_t* sc;
    int oom;
} agg_dense_shared_sel_t;
static void agg_dense_shared_sel_run(void* raw, const agg_view_t* v, uint32_t wid, int64_t ib, int64_t ie) {
    (void)wid;
    agg_dense_shared_sel_t* t = raw;
    agg_dense_shared_ctx_t* c = t->c;
    for (int64_t i = ib; i < ie; i++) {
        int64_t r = t->rows[i];
        int64_t k = agg_read_key_i64_raw(c->key->type, v->key_attrs[0], v->key_data[0], r - v->base);
        uint32_t slot = (uint32_t)agg_dense_component_raw(c->plan, 0, k);
        t->gid[i - ib] = slot;
        if (c->probe) { agg_probe_note(c, slot, r); continue; }
        if (c->cnt32) { __atomic_fetch_add(&c->cnt32[slot], 1, __ATOMIC_RELAXED); continue; }
        uint64_t bit = UINT64_C(1) << (slot % 64);
        _Atomic(uint64_t)* word = &c->occupied[slot / 64];
        if (!(atomic_load_explicit(word, memory_order_relaxed) & bit))
            atomic_fetch_or_explicit(word, bit, memory_order_relaxed);
    }
    if (c->cnt32 || c->probe) return;
    if (agg_sel_accum_chunk(c->vd, v, t->sc, c->states, t->gid, t->rows + ib, ie - ib, true) != 0) t->oom = 1;
}

/* Selected-row space [start, end): chunks of selected rows are decoded to
 * their original rows and read through the key sources (R-SPARSE for staged
 * ones). */
static void agg_dense_shared_sel_rows(agg_dense_shared_ctx_t* c, uint32_t wid, int64_t start, int64_t end) {
    const agg_desc_t* d = c->desc;
    int64_t rows[AGG_SEL_CHUNK];
    uint32_t gid[AGG_SEL_CHUNK];
    agg_sel_scratch_t sc;
    if (!agg_sel_scratch_init(&sc, c->n_aggs)) { ray_scan_fail_note(d->rr.fail, start, ray_error("oom", NULL)); return; }
    agg_curs_t cs; cs.cur = NULL;
    if (d->staged && !agg_curs_init(&cs, &d->rr)) {
        ray_scan_fail_note(d->rr.fail, start, ray_error("oom", NULL));
        agg_sel_scratch_free(&sc);
        return;
    }
    agg_sel_cursor_t cur;
    agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, start, end);
    agg_dense_shared_sel_t t = { c, rows, gid, &sc, 0 };
    int64_t cn;
    while (!t.oom && !agg_cancelled() && (cn = agg_sel_cursor_next(&cur, rows)) > 0) {
        if (!d->staged) { agg_dense_shared_sel_run(&t, &d->whole, wid, 0, cn); continue; }
        ray_t* err = agg_idx_walk(&d->rr, &cs, rows, 0, cn, &t, agg_dense_shared_sel_run, wid);
        if (err) { ray_scan_fail_note(d->rr.fail, start, err); break; }
    }
    if (t.oom) ray_scan_fail_note(d->rr.fail, start, ray_error("oom", NULL));
    agg_curs_free(&cs, &d->rr);
    agg_sel_scratch_free(&sc);
}

static void agg_dense_shared_rows(void* raw, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_shared_ctx_t* c = raw;
    if (c->sel) { agg_dense_shared_sel_rows(c, wid, start, end); return; }
    const agg_desc_t* d = c->desc;
    agg_curs_t cs; cs.cur = NULL;
    if (d->staged && !agg_curs_init(&cs, &d->rr)) { ray_scan_fail_note(d->rr.fail, start, ray_error("oom", NULL)); return; }
    for (int64_t begin = start; begin < end && !agg_cancelled(); begin += 8192) {
        int64_t limit = end - begin < 8192 ? end : begin + 8192;
        if (!d->staged) agg_dense_shared_block(c, &d->whole, wid, begin, limit);
        else if (!agg_walk_cs(&d->rr, &cs, c, agg_dense_shared_block, wid, begin, limit, start)) break;
    }
    agg_curs_free(&cs, &d->rr);
}

typedef struct { int64_t first, slot; bool strict; } agg_tie_t;
static int agg_tie_cmp(const void* a, const void* b) {
    int64_t x = ((const agg_tie_t*)a)->first, y = ((const agg_tie_t*)b)->first;
    return x < y ? -1 : x > y;
}
static int agg_dbl_cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

/* The kept groups of a top-N cut (the threshold's ties included) as the radix
 * route answers it: of the groups tied at the threshold only the ones with the
 * smallest first rows stay, so N groups remain, and the kept groups come in
 * first-row order, which the sort downstream keeps for equal values.  The
 * first rows come from one more pass over the keys (a probe pass) that notes
 * them for the kept slots only.  Kept values that are all different need
 * neither, and the slot order stands.  False with *err set on failure. */
static bool agg_dense_cut_ties(ray_pool_t* pool, agg_dense_shared_ctx_t* c, agg_desc_t* d, const agg_lean_src_t* src,
        const ray_group_emit_filter_t* ef, bool have_thr, double thr, int64_t rows,
        int64_t* slots, int64_t* n_io, ray_t** err) {
    int64_t n = *n_io;
    *err = NULL;
    if (n <= 1) return true;
    double* vals = ray_alloc_raw((size_t)n * sizeof(double));
    double* sorted = ray_alloc_raw((size_t)n * sizeof(double));
    uint64_t* bm = ray_calloc_raw(((size_t)c->slots + 63) / 64 * sizeof(uint64_t));
    int64_t* first = ray_alloc_raw((size_t)n * sizeof(int64_t));
    agg_tie_t* ties = ray_alloc_raw((size_t)n * sizeof(agg_tie_t));
    bool ok = false;
    if (!vals || !sorted || !bm || !first || !ties) { *err = ray_error("oom", NULL); goto done; }
    ok = true;
    if (!agg_lean_vals(src, slots, n, vals)) goto done;
    memcpy(sorted, vals, (size_t)n * sizeof(double));
    qsort(sorted, (size_t)n, sizeof(double), agg_dbl_cmp);
    bool distinct = true;
    for (int64_t i = 1; i < n && distinct; i++) distinct = sorted[i] != sorted[i - 1];
    if (distinct) goto done;
    for (int64_t i = 0; i < n; i++) {
        bm[slots[i] >> 6] |= UINT64_C(1) << (slots[i] & 63);
        first[i] = INT64_MAX;
    }
    c->probe = bm; c->pslots = slots; c->pn = n; c->pfirst = first;
    ray_pool_dispatch(pool, agg_dense_shared_rows, c, rows);
    c->probe = NULL;
    if (d->staged && (*err = ray_scan_fail_finish(&d->fail, pool)) != NULL) { ok = false; goto done; }
    if (agg_cancelled()) { *err = ray_error("cancel", NULL); ok = false; goto done; }
    int64_t nstrict = 0;
    for (int64_t i = 0; i < n; i++) {
        ties[i].first = first[i]; ties[i].slot = slots[i];
        ties[i].strict = !have_thr || (ef->desc ? vals[i] > thr : vals[i] < thr);
        nstrict += ties[i].strict;
    }
    int64_t room = have_thr ? ef->top_count_take - nstrict : n;
    qsort(ties, (size_t)n, sizeof(agg_tie_t), agg_tie_cmp);
    int64_t w = 0, tied = 0;
    for (int64_t i = 0; i < n; i++) {
        if (!ties[i].strict && tied++ >= room) continue;
        slots[w++] = ties[i].slot;
    }
    *n_io = w;
done:
    ray_free_raw(vals); ray_free_raw(sorted); ray_free_raw(bm); ray_free_raw(first); ray_free_raw(ties);
    return ok;
}

static ray_t* agg_dense_shared(ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext,
        ray_pool_t* pool, const dense_plan_t* plan, int64_t rows, ray_t* sel, const int64_t* sel_prefix,
        const agg_vo_t* vo, agg_desc_t* d, const ray_group_emit_filter_t* ef) {
    ray_t* ferr = NULL;   /* R-FAIL: a range error of a staged source */
    int64_t slots = plan->total_slots;
    agg_valdesc_t vd = { .n_aggs = ext->n_aggs, .vts = vo->vts, .off = vo->off, .block = vo->block,
        .val_data = d->val_data, .val_types = d->val_types, .val_hasnull = d->val_hasnull, .val_esz = d->val_esz,
        .val2_data = d->val2_data, .val2_types = d->val2_types, .val2_hasnull = d->val2_hasnull, .val2_esz = d->val2_esz };
    agg_dense_shared_ctx_t c = {.key = key_cols[0], .plan = plan, .layout = vo, .desc = d, .n_aggs = ext->n_aggs, .slots = slots,
        .sel = sel, .sel_prefix = sel_prefix, .vd = &vd};   /* rows: the selected rows with a selection */
    bool u32c = plan->wide && agg_count32_slab(vo->vts, ext->n_aggs, rows);
    c.states = ray_alloc_raw((size_t)slots * (u32c ? sizeof(uint32_t) : vo->block));
    if (u32c) c.cnt32 = (uint32_t*)c.states;
    c.occupied = ray_alloc_raw(((size_t)slots + 63) / 64 * sizeof(*c.occupied));
    if (!c.states || !c.occupied) goto failed;
    for (int64_t w = 0; w < (slots + 63) / 64; w++) atomic_init(&c.occupied[w], 0);
    ray_pool_dispatch(pool, u32c ? agg_dense_shared_zero : agg_dense_shared_init, &c, slots);
    ray_profile_tick("dense shared: initialized states");
    agg_desc_advise(d, rows);
    ray_pool_dispatch(pool, agg_dense_shared_rows, &c, rows);
    if (d->staged && (ferr = ray_scan_fail_finish(&d->fail, pool)) != NULL) goto failed;
    if (agg_cancelled()) goto failed;
    ray_profile_tick("dense shared: reduced rows");
    if (u32c) ray_pool_dispatch(pool, agg_dense_shared_cnt_occupied, &c, (slots + 63) / 64);
    route_stats.dense_strategy = AGG_DENSE_SHARED;
    route_stats.dense_tasks = ray_pool_total_workers(pool);
    route_stats.dense_local_slots = slots;
    route_stats.dense_count32 = u32c;
    int64_t* pre = NULL;
    int64_t pre_n = -1;
    if (ef && ef->top_count_take > 0) {
        agg_lean_src_t src = { .vt = vo->vts[ef->agg_index], .states = c.states, .block = vo->block,
            .off = vo->off[ef->agg_index], .param = ext->agg_k ? ext->agg_k[ef->agg_index] : 0,
            .cnt32 = u32c ? c.cnt32 : NULL };
        bool have_thr;
        double thr;
        int64_t k = agg_dense_lean_select(pool, &src, (const uint64_t*)c.occupied, slots, ef, &pre, &have_thr, &thr);
        if (k >= 0) {
            if (!agg_dense_cut_ties(pool, &c, d, &src, ef, have_thr, thr, rows, pre, &k, &ferr)) {
                ray_free_raw(pre);
                goto failed;
            }
            pre_n = k;
        } else if (u32c) {
            goto failed;
        }
    }
    return agg_dense_finish(key_cols, key_syms, ext, pool, vo, d, slots, c.states, NULL, (const uint64_t*)c.occupied, plan, slots, 0, ef, 0, plan, u32c, pre, pre_n);
failed:
    ray_free_raw(c.states); ray_free_raw(c.occupied);
    return ferr ? ferr : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
}

/* The dense phase A may count on the key's runs (agg_dense_runs_*): one
 * staged generation-2 integer or SYM key, a raw-range plan, every
 * aggregate COUNT (D6: the count adds a run's rows; any other aggregate
 * needs its rows' values). */
static bool agg_dense_runs_ok(const agg_rr_t* rr, const dense_plan_t* dp, const agg_vtable_t** vts,
                              uint32_t n_keys, uint32_t n_aggs) {
    if (n_keys != 1 || !rr->staged || !rr->srcs[0] || !ray_col_src_encoded(rr->srcs[0]) || dp->compacted) return false;
    switch (rr->key_types[0]) {
    case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8: case RAY_BOOL:
    case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: case RAY_SYM: break;
    default: return false;
    }
    const agg_vtable_t* cnt = agg_resolve(OP_COUNT, RAY_I64);
    for (uint32_t a = 0; a < n_aggs; a++)
        if (vts[a] != cnt || vts[a]->state_size != sizeof(int64_t)) return false;
    return true;
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
    ray_t* derr = NULL;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols, &derr)) {
        agg_vo_free(&vo);
        return derr ? derr : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    const void** key_data = d.key_data;
    const void** val_data = d.val_data; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_data; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;

    if (strategy == AGG_DENSE_SHARED) {
        ray_t* result = agg_dense_shared(key_cols, key_syms, ext, pool, dp, sel ? n_sel : nrows, sel, sel_prefix, &vo, &d, efp);
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
    /* The eager-initialization samples read a staged key through cursors
     * (R-SPARSE: strided single rows, ascending). */
    agg_curs_t scs; scs.cur = NULL;
    ray_t* serr = NULL;
    if (d.staged && !sel && n_keys == 1 && !agg_curs_init(&scs, &d.rr)) serr = ray_error("oom", NULL);
    for (uint32_t w = 0; w < nw && !serr; w++) {
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
            for (int64_t sample = 0; sample < 64 && !serr; sample++) {
                int64_t row = begin + (end - begin - 1) * sample / 63;
                int64_t key = d.key_srcs[0] ? agg_key_at(&d.rr, &scs, 0, row, &serr)
                                            : agg_read_key_i64(key_cols[0], key_data[0], row);
                if (serr) break;                 /* key is 0: never a slot (a remap would read past it) */
                int64_t slot = agg_dense_component(dp, 0, key);
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
    agg_curs_free(&scs, &d.rr);
    if (serr) { ray_free_raw(locals); agg_vo_free(&vo); agg_desc_free(&d); return serr; }
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
        .rr = &d.rr, .whole = &d.whole,
        .runs = agg_dense_runs_ok(&d.rr, dp, vts, n_keys, n_aggs),
    };
    /* Sel mode dispatches over the SELECTED-row space [0,n_sel); each worker
     * range maps to a segment span via sel_prefix.  Non-sel dispatches over rows. */
    agg_desc_advise(&d, nrows);
    ray_pool_dispatch_n(pool, ctx.runs ? agg_dense_task_runs_fn : agg_dense_task_fn, &ctx, nw);
    ray_profile_tick("dense: accumulated rows");

    ray_t* ferr = d.staged ? ray_scan_fail_finish(&d.fail, pool) : NULL;
    for (uint32_t w = 0; w < nw; w++)
        if (ferr || locals[w].oom || agg_cancelled()) {
            for (uint32_t i = 0; i < nw; i++)
                agg_dense_local_destroy_states(&locals[i], vts, off, block, n_aggs);
            ray_free_raw(locals);
            agg_vo_free(&vo); agg_desc_free(&d);
            return ferr ? ferr : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
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
                                      total_slots, gstates, gfirst, NULL, NULL, 0, 0, efp, group_limit, dp, false, NULL, -1);
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
/* A key column of `ng` rows from key words at w[g * stride]: the type, SYM
 * width and domain of `like` (the key column or its stand-in).  A word
 * equal to the type's null sentinel is a null row and sets HAS_NULLS (so
 * does a nullable `like`, as ray_group_gather would); BOOL and U8 are never
 * null.  Caller owns the column. */
static ray_t* agg_keycol_from_words(ray_t* like, const int64_t* w, size_t stride, int64_t ng) {
    ray_t* out = col_vec_new(like, ng);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM) ray_sym_vec_adopt_domain(out, sym_domain_rep(like));
    out->len = ng;
    bool nulls = ray_vec_may_have_nulls(like);
    int64_t null = like->type == RAY_SYM ? 0 : agg_key_null(like->type);
    bool nullable_type = like->type != RAY_BOOL && like->type != RAY_U8;
    void* dst = ray_data(out);
    for (int64_t g = 0; g < ng; g++) {
        int64_t v = w[(size_t)g * stride];
        if (nullable_type && v == null) nulls = true;
        write_col_i64(dst, g, v, out->type, out->attrs);
    }
    if (nulls) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

typedef struct {
    int32_t* ht;          /* [htcap] slot -> local gid, -1 empty */
    int64_t  htcap;
    uint64_t htmask;
    int64_t* first_row;   /* [cap] representative (MIN) row idx per local group */
    char*    states;      /* [cap*block] AoS group state */
    int64_t* gkey;        /* staged keys: [cap * n_keys] key words per group; else NULL */
    uint32_t n_keys;      /* words per gkey entry (0: first-row compares) */
    int64_t  ng, cap;
    size_t   block;
    int      oom;
} agg_sh_t;

/* n_words > 0: the table keys its groups by stored key words (staged keys,
 * read by range, have no row to compare at later). */
static int agg_sh_init(agg_sh_t* sh, int64_t cap, size_t block, uint32_t n_words) {
    if (cap < 1) cap = 1;
    int64_t htcap = 2;
    while (htcap < cap * 2) htcap <<= 1;   /* <0.5 load at full cap */
    sh->ng = 0; sh->cap = cap; sh->block = block; sh->oom = 0;
    sh->htcap = htcap; sh->htmask = (uint64_t)htcap - 1;
    sh->n_keys = n_words;
    sh->ht        = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
    sh->first_row = ray_alloc_raw((size_t)cap * sizeof(int64_t));
    sh->states    = ray_alloc_raw((size_t)cap * block);
    sh->gkey      = n_words ? ray_alloc_raw((size_t)cap * n_words * sizeof(int64_t)) : NULL;
    if (!sh->ht || !sh->first_row || !sh->states || (n_words && !sh->gkey)) { sh->oom = 1; return -1; }
    for (int64_t i = 0; i < htcap; i++) sh->ht[i] = -1;
    return 0;
}

static void agg_sh_destroy(agg_sh_t* sh) {
    ray_free_raw(sh->ht); ray_free_raw(sh->first_row); ray_free_raw(sh->states); ray_free_raw(sh->gkey);
    sh->ht = NULL; sh->first_row = NULL; sh->states = NULL; sh->gkey = NULL;
}

/* Grow capacity (states + first_row, and the key words) when ng would exceed
 * cap.  Doubles cap. */
static int agg_sh_grow_cap(agg_sh_t* sh) {
    int64_t nc = sh->cap * 2;
    int64_t* nfr = ray_realloc_raw(sh->first_row, (size_t)nc * sizeof(int64_t));
    if (!nfr) { sh->oom = 1; return -1; }
    sh->first_row = nfr;
    char* nst = ray_realloc_raw(sh->states, (size_t)nc * sh->block);
    if (!nst) { sh->oom = 1; return -1; }
    sh->states = nst;
    if (sh->n_keys) {
        int64_t* ngk = ray_realloc_raw(sh->gkey, (size_t)nc * sh->n_keys * sizeof(int64_t));
        if (!ngk) { sh->oom = 1; return -1; }
        sh->gkey = ngk;
    }
    sh->cap = nc;
    return 0;
}

/* Rehash the hash table to a larger htcap (keeps load factor low).  The gid set
 * is unchanged; we recompute each group's slot from its representative row's
 * tuple hash, or from its stored key words (never reading a column).
 * n_keys is uint32_t (unbounded), same contract as agg_tuple_hash. */
static int agg_sh_grow_ht(agg_sh_t* sh, ray_t** key_cols, const void** key_data,
                          uint32_t n_keys) {
    int64_t nc = sh->htcap * 2;
    int32_t* nht = ray_alloc_raw((size_t)nc * sizeof(int32_t));
    if (!nht) { sh->oom = 1; return -1; }
    uint64_t nmask = (uint64_t)nc - 1;
    for (int64_t i = 0; i < nc; i++) nht[i] = -1;
    for (int64_t g = 0; g < sh->ng; g++) {
        uint64_t h = sh->n_keys ? agg_words_hash(&sh->gkey[(size_t)g * sh->n_keys], sh->n_keys)
                                : agg_tuple_hash(key_cols, key_data, n_keys, sh->first_row[g]);
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
    const agg_rr_t*    rr;         /* sources; staged ones are read by range */
    const agg_view_t*  whole;
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

/* Probe-or-insert by key words w[0, sh->n_keys) (a staged table; `r` the
 * candidate's first row, kept as the MIN).  Keys reaching the small hash
 * are integer, temporal, BOOL, U8 or SYM, so a widened word identifies its
 * value exactly.  Returns gid (>=0) or -1 on oom. */
static int32_t agg_sh_find_or_insert_words(
        agg_sh_t* sh, const int64_t* w, const agg_vtable_t** vts, const size_t* off,
        uint32_t n_aggs, int64_t r) {
    uint32_t nk = sh->n_keys;
    uint64_t h = agg_words_hash(w, nk);
    uint64_t slot = h & sh->htmask;
    for (;;) {
        int32_t gp = sh->ht[slot];
        if (gp < 0) {                       /* new group */
            if (sh->ng == sh->cap && agg_sh_grow_cap(sh) != 0) return -1;
            if ((sh->ng + 1) * 2 > sh->htcap) {
                if (agg_sh_grow_ht(sh, NULL, NULL, 0) != 0) return -1;
                slot = h & sh->htmask;
                while (sh->ht[slot] >= 0) slot = (slot + 1) & sh->htmask;
            }
            int32_t g = (int32_t)sh->ng;
            sh->ht[slot] = g;
            sh->first_row[g] = r;
            memcpy(&sh->gkey[(size_t)g * nk], w, (size_t)nk * sizeof(int64_t));
            for (uint32_t a = 0; a < n_aggs; a++)
                vts[a]->init(sh->states + (size_t)g * sh->block + off[a]);
            sh->ng++;
            return g;
        }
        if (memcmp(&sh->gkey[(size_t)gp * nk], w, (size_t)nk * sizeof(int64_t)) == 0) {
            if (r < sh->first_row[gp]) sh->first_row[gp] = r;   /* MIN representative */
            return gp;
        }
        slot = (slot + 1) & sh->htmask;
    }
}

/* Group id of row r read through view v: by first-row compares over whole
 * columns, by key words over pieces. */
static inline int32_t agg_sh_gid(agg_sh_ctx_t* c, agg_sh_t* sh, const agg_view_t* v, int64_t r) {
    if (!sh->n_keys)
        return agg_sh_find_or_insert(sh, c->key_cols, c->key_data, c->n_keys,
                                     c->vts, c->off, c->n_aggs, r);
    uint32_t nk = c->n_keys;
    int64_t w[nk];
    for (uint32_t k = 0; k < nk; k++) w[k] = agg_vkey(v, c->rr->key_types, k, r);
    return agg_sh_find_or_insert_words(sh, w, c->vts, c->off, c->n_aggs, r);
}

/* Phase A body: rows [start, end) (at most AGG_SH_CHUNK) through view v. */
static void agg_sh_rows(void* vctx, const agg_view_t* v, uint32_t wid, int64_t cs, int64_t ce) {
    agg_sh_ctx_t* c = (agg_sh_ctx_t*)vctx;
    agg_sh_t* sh = &c->locals[wid];
    if (sh->oom) return;
    uint32_t gid[AGG_SH_CHUNK];
    int64_t n = ce - cs;
    const int64_t base = v->base;
    for (int64_t r = cs; r < ce; r++) {
        int32_t g = agg_sh_gid(c, sh, v, r);
        if (g < 0) { sh->oom = 1; return; }
        gid[r - cs] = (uint32_t)g;
    }
    for (uint32_t a = 0; a < c->n_aggs; a++) {
        if (c->vts[a]->update_batch2) {             /* binary agg (pearson) */
            const void* vx = (const char*)v->val_data[a]  + (size_t)(cs - base) * c->val_esz[a];
            const void* vy = (const char*)v->val2_data[a] + (size_t)(cs - base) * c->val2_esz[a];
            ray_valid_t valid_x = { vx, c->val_types[a],  c->val_hasnull[a] };
            ray_valid_t valid_y = { vy, c->val2_types[a], c->val2_hasnull[a] };
            c->vts[a]->update_batch2(sh->states + c->off[a], c->block, gid,
                                     vx, vy, &valid_x, &valid_y, n, NULL);
        } else {
            const void* vals = (v->val_data[a])
                ? (const void*)((const char*)v->val_data[a] + (size_t)(cs - base) * c->val_esz[a])
                : NULL;
            ray_valid_t valid = { vals, c->val_types[a],
                                  v->val_data[a] ? c->val_hasnull[a] : false };
            c->vts[a]->update_batch(sh->states + c->off[a], c->block,
                                    gid, vals, &valid, n, NULL);
        }
    }
}

/* Phase A, sel mode: one run rows[ib, ie) of a selected chunk. */
typedef struct { agg_sh_ctx_t* c; agg_sh_t* sh; const int64_t* rows; uint32_t* gid; agg_sel_scratch_t* sc; } agg_sh_sel_run_t;
static void agg_sh_sel_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t ib, int64_t ie) {
    (void)wid;
    agg_sh_sel_run_t* t = raw;
    if (t->sh->oom) return;
    for (int64_t i = ib; i < ie; i++) {
        int32_t g = agg_sh_gid(t->c, t->sh, v, t->rows[i]);
        if (g < 0) { t->sh->oom = 1; return; }
        t->gid[i - ib] = (uint32_t)g;
    }
    if (agg_sel_accum_chunk(&t->c->vd, v, t->sc, t->sh->states, t->gid, t->rows + ib, ie - ib, false) != 0)
        t->sh->oom = 1;
}

/* Phase A: per-worker small-hash group + accumulate over chunk [start,end),
 * processed in fixed-size sub-chunks so the gid buffer is O(CHUNK), not O(N). */
static void agg_sh_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_sh_ctx_t* c = (agg_sh_ctx_t*)vctx;
    agg_sh_t* sh = &c->locals[wid];
    if (sh->oom) return;
    const agg_rr_t* rr = c->rr;

    /* ── Sel mode: chunk-iterate the SELECTED rows in [start,end) (selected-row
     * space), decoding ORIGINAL rows, probing the per-worker hash, gathering
     * agg values per chunk into reused scratch.  No full index/compact alloc. */
    if (c->sel) {
        int64_t rows[AGG_SEL_CHUNK];
        uint32_t sgid[AGG_SEL_CHUNK];
        agg_sel_scratch_t sc;
        if (!agg_sel_scratch_init(&sc, c->n_aggs)) { sh->oom = 1; return; }
        agg_curs_t cs; cs.cur = NULL;
        if (rr->staged) {
            if (ray_scan_fail_skip(rr->fail, start)) { agg_sel_scratch_free(&sc); return; }
            if (!agg_curs_init(&cs, rr)) { sh->oom = 1; agg_sel_scratch_free(&sc); return; }
        }
        agg_sel_cursor_t cur;
        agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, start, end);
        agg_sh_sel_run_t t = { c, sh, rows, sgid, &sc };
        int64_t cn;
        while (!sh->oom && (cn = agg_sel_cursor_next(&cur, rows)) > 0) {
            if (!rr->staged) { agg_sh_sel_rows(&t, c->whole, wid, 0, cn); continue; }
            ray_t* err = agg_idx_walk(rr, &cs, rows, 0, cn, &t, agg_sh_sel_rows, wid);
            if (err) { ray_scan_fail_note(rr->fail, start, err); break; }
        }
        agg_curs_free(&cs, rr);
        agg_sel_scratch_free(&sc);
        return;
    }

    agg_curs_t tc; tc.cur = NULL;
    if (rr->staged && !agg_curs_init(&tc, rr)) { sh->oom = 1; return; }
    for (int64_t cs = start; cs < end && !sh->oom; cs += AGG_SH_CHUNK) {
        int64_t ce = cs + AGG_SH_CHUNK; if (ce > end) ce = end;
        if (!rr->staged) agg_sh_rows(c, c->whole, wid, cs, ce);
        else if (!agg_walk_cs(rr, &tc, c, agg_sh_rows, wid, cs, ce, start)) break;
    }
    agg_curs_free(&tc, rr);
}

/* Parallel small-hash fallback.  Precondition: int/SYM keys and streaming
 * accumulators. Hashes start at one group and grow from observed cardinality.
 * With staged keys every table keys its groups by key words: the merge
 * inserts a local group by its words and the emit writes the keys from
 * them, so no column is read at a stored row. */
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
    ray_t* derr = NULL;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols, &derr)) return derr ? derr : ray_error("oom", NULL);
    const void** key_data = d.key_data;
    const void** val_data = d.val_data; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_data; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;
    const int64_t* agg_syms = d.agg_syms;
    bool words = false;
    for (uint32_t k = 0; k < n_keys; k++) words |= d.key_srcs[k] != NULL;
    uint32_t n_words = words ? n_keys : 0;

    agg_sh_t* locals = ray_calloc_raw((size_t)((size_t)nw) * (sizeof(agg_sh_t)));
    if (!locals) { agg_desc_free(&d); return ray_error("oom", NULL); }
    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++)
        if (agg_sh_init(&locals[w], 1, block, n_words) != 0) { alloc_oom = 1; break; }
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
        .rr = &d.rr, .whole = &d.whole,
    };
    agg_desc_advise(&d, nrows);
    ray_pool_dispatch(pool, agg_sh_phaseA_fn, &ctx, sel ? n_sel : nrows);
    ray_t* ferr = d.staged ? ray_scan_fail_finish(&d.fail, pool) : NULL;

    for (uint32_t w = 0; w < nw; w++)
        if (ferr || locals[w].oom) {
            for (uint32_t i = 0; i < nw; i++) agg_sh_destroy(&locals[i]);
            ray_free_raw(locals);
            agg_desc_free(&d);
            return ferr ? ferr : ray_error("oom", NULL);
        }

    /* ── Phase B: merge per-worker hashes into a global hash (serial). ──
     * Entries ≤ groups×nworkers, so size the global hash to that bound. */
    int64_t gcap = 0;
    for (uint32_t w = 0; w < nw; w++) gcap += locals[w].ng;
    agg_sh_t gt = {0};
    if (agg_sh_init(&gt, gcap, block, n_words) != 0) {
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
            int32_t gg = n_words
                ? agg_sh_find_or_insert_words(&gt, &loc->gkey[(size_t)lg * n_words], vts, off, n_aggs, fr)
                : agg_sh_find_or_insert(&gt, key_cols, key_data, n_keys, vts, off, n_aggs, fr);
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
     * trivially cheap for the few groups this path handles; a word-keyed
     * table writes its keys from the words. */
    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        agg_sh_destroy(&gt); agg_desc_free(&d);
        return result ? result : ray_error("oom", NULL);
    }
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = n_words ? agg_keycol_from_words(key_cols[k], gt.gkey + k, n_words, ng)
                            : ray_group_gather(key_cols[k], gt.first_row, ng);
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
 *   Phase 1 (parallel over fixed input chunks): two-pass radix partitioning
 *            with exact offsets.  Pass 1 counts, per (chunk, partition), the
 *            rows hashing to each partition; a prefix sum turns the counts into
 *            each (chunk, partition)'s exact slot range inside ONE payload
 *            array allocated once; pass 2 reads back each row's partition id
 *            (stored by pass 1) and writes its packed record at its slot.
 *            Partition p's records are contiguous and in ascending input
 *            order (chunk order, then row order).
 *   Phase 2 (parallel over partitions): walk partition p's contiguous
 *            records; build a small open-addressing hash over those rows
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

/* PAYLOAD layout.  Phase 1 scatters one fixed-size record per row instead of a
 * bare row index: the record packs [n_keys×int64 widened keys][agg input
 * value(s) at native esz][row_idx int64].  Phase 2 then groups+accumulates each
 * partition by walking its records SEQUENTIALLY — hash/equality compare the
 * contiguous packed keys and accumulation reads values from the contiguous
 * record — trading a one-time scatter cost for cache-friendly Phase-2 reads
 * (the memory-bound hot spot was agg_tuple_eq re-reading scattered key columns
 * on every distinct insert).
 *
 * All records live in ONE array of input_count records, partition-major:
 * partition p owns records [part_start[p], part_start[p+1]).  Inside p, the
 * slots are carved per input chunk in chunk order, so a partition's records
 * appear in ascending input order no matter which worker ran which chunk.
 * The allocation count is O(1) in the worker count: no per-(worker,
 * partition) buffers to grow, and nothing for the dispatcher to free one by
 * one. */

/* Input rows per phase-1 chunk, at least.  Chunks are the unit of both the
 * counting and the scatter pass, and the count matrix holds one uint32 per
 * (chunk, partition), so chunks are few (a handful per worker) but never so
 * small that the per-chunk partition row dwarfs the rows it counts. */
#define AGG_RADIX_CHUNK_MIN_ROWS  16384
/* ...but an input of at least this many rows per worker still gives every
 * worker a chunk, so mid-size inputs count and scatter on the whole pool. */
#define AGG_RADIX_CHUNK_FLOOR_ROWS 4096
#define AGG_RADIX_CHUNKS_PER_WORKER 4
/* The fused pass's chunks (generation 2) hold this many records per
 * partition on average: smaller chunks keep the staged records in cache,
 * fewer records per (chunk, partition) run make phase 2's reads scatter. */
#define AGG_RADIX_RUN_ROWS 8
/* Phase 2 of a fused context prefetches the run this many chunks ahead. */
#define AGG_RADIX_RUN_AHEAD 4
/* Count-matrix budget: chunks are reduced (never below one per worker) so
 * n_chunks × n_parts × 4 bytes stays under this. */
#define AGG_RADIX_COUNT_BUDGET  ((size_t)64 << 20)
/* Partitions per prefix-sum task (one cache-line-friendly band of each
 * chunk's count row). */
#define AGG_RADIX_PREFIX_BAND   64

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

/* A record field of a compact fused context (agg_radix_ctx_t.cf): where
 * the scatter takes it from.  A staged source's field is copied into the
 * task's staged row at soff; a plain source's (a column read in place) is
 * read by the scatter at its row, as the two passes' scatter reads it. */
enum { AGG_CF_STAGED, AGG_CF_KEY_I64, AGG_CF_KEY_I32, AGG_CF_KEY_I16, AGG_CF_KEY_U8,
       AGG_CF_KEY_U16, AGG_CF_KEY_U32, AGG_CF_VAL };
typedef struct {
    const char* plain;      /* plain fields: the column's row 0 */
    uint32_t    off;        /* the field's offset in the record */
    uint32_t    soff;       /* staged fields: the offset in the staged row */
    uint32_t    agg;        /* inputs: the aggregate whose input this is */
    uint8_t     y;          /* inputs: 1 for the y side of a binary aggregate */
    uint8_t     w;          /* bytes in the record (keys: 8) */
    uint8_t     esz;        /* plain fields: the column's element width */
    uint8_t     kind;       /* AGG_CF_* */
} agg_radix_cf_t;

/* Spill of the later waves of a reduced radix run (agg_radix_spill_*).  One
 * unlinked file in the heap's spill directory; each worker fills one buffer
 * per wave and appends it whole when full — a block of n records followed
 * by their n uint32 payload slots inside the wave — at a file offset taken
 * from one shared tail, so every write is a large sequential pwrite.  A wave
 * is read back block by block (pread) straight into the payload slots, then
 * reduced like any other wave.  Written once, read once, unlinked at once. */
typedef struct { int64_t off; uint32_t n; uint32_t wave; } agg_radix_blk_t;
typedef struct { char* buf; uint32_t n; } agg_radix_sbuf_t;
typedef struct agg_radix_spill_s {
    int                fd;
    uint32_t           n_waves;
    uint32_t           bcap;       /* records per buffer */
    size_t             rec;
    const int64_t*     wave_base;  /* [n_waves] first record of each wave */
    agg_radix_sbuf_t*  bufs;       /* [nw * n_waves]; wave 0's unused */
    char*              rbuf;       /* [nw] read-back buffers */
    size_t             rstride;    /* bytes per buffer, 8-aligned */
    agg_radix_blk_t*   blks;
    int64_t            blk_cap;
    const int64_t*     wave_blks;  /* indices into blks of the wave read back */
    _Atomic(int64_t)   n_blks;
    _Atomic(int64_t)   tail;
    _Atomic(int)       err;
} agg_radix_spill_t;

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
    /* Phase-1 chunking of the input space [0, n_in) (selected-row space in
     * sel mode): chunk c covers [c*chunk_rows, min((c+1)*chunk_rows, n_in)). */
    int64_t             n_in;
    int64_t             chunk_rows;
    uint32_t            n_chunks;
    /* [n_chunks * n_parts]: pass 1 stores per-(chunk, partition) row counts;
     * the prefix pass turns them into each chunk's offset inside partition
     * p's slot range; pass 2 uses its chunk's row as write cursors. */
    uint32_t*           cnt;
    int64_t*            part_start; /* [n_parts + 1] record index of p's slots */
    char*               pay;        /* [n_in * rec] payload records */
    uint16_t*           pid;        /* [n_in] partition of each input position
                                     * (pass 1 writes, pass 2 reads) */
    agg_radix_part_t*   parts;      /* [n_parts] */
    _Atomic(int)        part_overflow; /* a partition exceeds UINT32_MAX rows */
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
    const agg_rr_t*     rr;         /* sources; staged ones are read by range */
    const agg_view_t*   whole;
    /* Chunk-major payload (a staged context whose sources decode; chunks
     * sized by exec_group_v2_parallel_radix): a task reads its chunk once,
     * staging its records, and scatters them inside the chunk's own record
     * range [chunk start, chunk end).  cnt then holds each (chunk,
     * partition)'s offset inside that range, runs the same transposed with
     * the lengths, part_start the partition totals' prefix; pid is unused.
     * A partition's records are its chunks' runs in chunk order: the global
     * layout's sequence, so phase 2 groups the same records in the same
     * order (agg_radix_group_fused_fn). */
    uint32_t            fused_tasks; /* fused: tasks of consecutive chunks */
    const agg_rr_t*     count_rr;   /* two passes: the sources pass 1 reads (the
                                     * keys; rr's other staged sources dropped) */
    uint32_t*           runs;       /* fused: [n_parts][n_chunks] (offset inside the
                                     * chunk, length) of each partition's run, the
                                     * count matrix transposed for phase 2 */
    /* Compact staging (fused, not sel mode): a task stages only the staged
     * sources' fields (srec bytes a row: keys widened, inputs at their
     * widths) and the scatter builds each record from them, the plain
     * columns read in place and the position.  cf[f] is field f in the
     * record's order: the keys (cf[k] is key k's), then the inputs present,
     * each aggregate's x then its y (n_cf in all).  Not compact: a task
     * stages whole records. */
    bool                compact;
    size_t              srec;
    uint32_t            n_cf;
    const agg_radix_cf_t* cf;
    /* Waves (agg_radix_wave_plan): the payload holds the records of the
     * partitions [wave_lo, wave_hi) only, partition p's slots starting at
     * record part_start[p] - pay_base; phase 2 runs partition part_lo + task.
     * One pass over every partition is wave 0 = [0, n_parts), base 0. */
    uint32_t            wave_lo, wave_hi;
    uint32_t            part_lo;
    int64_t             pay_base;
    /* Top-N candidates (agg_radix_part_compact): a finished partition with
     * more than cmp_cap groups keeps only its cmp_cap best by the filter
     * aggregate, ties to the smaller first row.  0 = keep every group. */
    int64_t             cmp_cap;
    const agg_vtable_t* cmp_vt;
    size_t              cmp_off;
    int64_t             cmp_param;
    uint8_t             cmp_desc;
    _Atomic(int64_t)    cmp_parts;  /* partitions reduced (route stats) */
    /* Spilled waves (agg_radix_spill_scatter_fn): wave_of[p] is partition
     * p's wave; wave 0 scatters into the payload, the later waves into
     * per-(worker, wave) buffers written to the spill file. */
    const uint16_t*     wave_of;
    struct agg_radix_spill_s* spill;
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

/* agg_read_key_i64 for the plain radix loops, the column's attrs read only
 * by a SYM key (agg_read_key_i64 passes them to the raw reader up front,
 * and the scatter loop then loads them for every key of every row). */
RAY_INLINE int64_t agg_radix_key_at(const ray_t* col, const void* data, int64_t row) {
    switch (col->type) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: return ((const int64_t*)data)[row];
        case RAY_I32: case RAY_DATE: case RAY_TIME: return ((const int32_t*)data)[row];
        case RAY_I16: return ((const int16_t*)data)[row];
        case RAY_U8:  case RAY_BOOL: return ((const uint8_t*)data)[row];
        case RAY_SYM: return (int64_t)ray_read_sym(data, row, RAY_SYM, col->attrs);
        default: return 0;  /* the gate admits only the above */
    }
}

/* Pack the payload record of ORIGINAL row r at input position `pos` of a
 * plain context (whole columns). */
RAY_INLINE void agg_radix_put(const agg_radix_ctx_t* c, char* rec, int64_t r, int64_t pos) {
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    int64_t* kdst = (int64_t*)rec;
    for (uint32_t k = 0; k < n_keys; k++)
        kdst[k] = agg_radix_key_at(c->key_cols[k], c->key_data[k], r);
    int64_t input_order = pos;
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
}

/* agg_radix_put of a staged context: row r read through view v (keys at
 * r - base with the pieces' SYM widths, inputs from the pieces). */
RAY_INLINE void agg_radix_put_view(const agg_radix_ctx_t* c, const agg_view_t* v, char* rec,
                                   int64_t r, int64_t pos) {
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    const int64_t i = r - v->base;
    int64_t* kdst = (int64_t*)rec;
    for (uint32_t k = 0; k < n_keys; k++)
        kdst[k] = agg_read_key_i64_raw(c->key_cols[k]->type, v->key_attrs[k], v->key_data[k], i);
    int64_t input_order = pos;
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (v->val_data[a]) {
            uint8_t ez = c->val_esz[a];
            memcpy(rec + c->val_off[a], (const char*)v->val_data[a] + (size_t)i * ez, ez);
        }
        if (v->val2_data[a]) {
            uint8_t ez2 = c->val2_esz[a];
            memcpy(rec + c->val2_off[a], (const char*)v->val2_data[a] + (size_t)i * ez2, ez2);
        }
    }
    if (c->needs_row)
        memcpy(rec + c->row_off, &input_order, sizeof(input_order));
}

/* Visit one ORIGINAL row r at input position `pos`.
 *
 * Pass 1 (scatter == false) hashes the key tuple, records the partition in
 * pid[pos] and counts the row in its partition's cell of this chunk's count
 * row.  Pass 2 (scatter == true) reads the partition back from pid[pos] and
 * writes the packed record at the chunk's next slot of that partition; in a
 * wave (`wave`) only a partition of [wave_lo, wave_hi) is written, at its
 * slot less pay_base.
 *
 * Storing the partition id (2 bytes per row, freed after pass 2) instead of
 * re-hashing in pass 2 measured 15-25% faster for phase 1: the destination
 * address no longer waits on the key load -> FNV -> fmix chain, so the
 * scattered stores issue back to back, and the keys go straight from the
 * column into the record with no staging row. */
RAY_INLINE void agg_radix_visit(agg_radix_ctx_t* c, uint32_t* cur,
                                int64_t r, int64_t pos, bool scatter, bool wave) {
    uint32_t n_keys = c->n_keys;
    if (!scatter) {
        uint64_t h = 1469598103934665603ULL;
        for (uint32_t k = 0; k < n_keys; k++) {
            int64_t v = agg_radix_key_at(c->key_cols[k], c->key_data[k], r);
            h ^= (uint64_t)v; h *= 1099511628211ULL;
        }
        h = agg_radix_fmix64(h);
        uint32_t p = (uint32_t)(h & (c->n_parts - 1));
        c->pid[pos] = (uint16_t)p;
        cur[p]++;
        return;
    }
    uint32_t p = c->pid[pos];
    if (wave && (p < c->wave_lo || p >= c->wave_hi)) return;
    agg_radix_put(c, c->pay + (size_t)(c->part_start[p] - c->pay_base + cur[p]++) * c->rec, r, pos);
}

/* agg_radix_visit of a staged context: row r read through view v (keys at
 * r - base with the pieces' SYM widths, inputs from the pieces).  The plain
 * loops keep the column form above (its whole-column pointers are the
 * context's; their loads stay as they were in the hot loop). */
RAY_INLINE void agg_radix_visit_view(agg_radix_ctx_t* c, const agg_view_t* v, uint32_t* cur,
                                     int64_t r, int64_t pos, bool scatter, bool wave) {
    uint32_t n_keys = c->n_keys;
    if (!scatter) {
        const int64_t i = r - v->base;
        uint64_t h = 1469598103934665603ULL;
        for (uint32_t k = 0; k < n_keys; k++) {
            int64_t val = agg_read_key_i64_raw(c->key_cols[k]->type, v->key_attrs[k], v->key_data[k], i);
            h ^= (uint64_t)val; h *= 1099511628211ULL;
        }
        h = agg_radix_fmix64(h);
        uint32_t p = (uint32_t)(h & (c->n_parts - 1));
        c->pid[pos] = (uint16_t)p;
        cur[p]++;
        return;
    }
    uint32_t p = c->pid[pos];
    if (wave && (p < c->wave_lo || p >= c->wave_hi)) return;
    agg_radix_put_view(c, v, c->pay + (size_t)(c->part_start[p] - c->pay_base + cur[p]++) * c->rec, r, pos);
}

/* The fused pass's visit of row r at position pos through view v: the
 * hash picks the partition (recorded in the task's pid and counted, as
 * pass 1 does) and the packed record is written at the position's slot of
 * the task's staging buffer (as pass 2 writes it), so every source is read
 * once.  stage and spid are indexed by pos - pos0, the chunk's first
 * position. */
RAY_INLINE void agg_radix_stage_view(agg_radix_ctx_t* c, const agg_view_t* v, uint32_t* cur,
                                     char* stage, uint16_t* spid, int64_t pos0, int64_t r, int64_t pos) {
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    const int64_t i = r - v->base;
    char* rec = stage + (size_t)(pos - pos0) * c->rec;
    int64_t* kdst = (int64_t*)rec;
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t val = agg_read_key_i64_raw(c->key_cols[k]->type, v->key_attrs[k], v->key_data[k], i);
        kdst[k] = val;
        h ^= (uint64_t)val; h *= 1099511628211ULL;
    }
    h = agg_radix_fmix64(h);
    uint32_t p = (uint32_t)(h & (c->n_parts - 1));
    spid[pos - pos0] = (uint16_t)p;
    cur[p]++;
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (v->val_data[a]) {
            uint8_t ez = c->val_esz[a];
            memcpy(rec + c->val_off[a], (const char*)v->val_data[a] + (size_t)i * ez, ez);
        }
        if (v->val2_data[a]) {
            uint8_t ez2 = c->val2_esz[a];
            memcpy(rec + c->val2_off[a], (const char*)v->val2_data[a] + (size_t)i * ez2, ez2);
        }
    }
    if (c->needs_row) {
        int64_t input_order = pos;
        memcpy(rec + c->row_off, &input_order, sizeof(input_order));
    }
}

/* One chunk's pass over a piece or a run, through view v: rows [b, e)
 * (positions pos0 + (r - b)), or the selected rows rows[ib, ie) at
 * positions pos0 + ib. */
typedef struct {
    agg_radix_ctx_t* c;
    uint32_t*        cur;
    const int64_t*   rows;      /* sel mode: the chunk's decoded rows */
    int64_t          pos0;
} agg_radix_run_t;
RAY_INLINE void agg_radix_run_rows(void* raw, const agg_view_t* v, int64_t b, int64_t e,
                                   bool scatter, bool wave) {
    agg_radix_run_t* t = raw;
    if (t->rows) {
        for (int64_t i = b; i < e; i++)
            agg_radix_visit_view(t->c, v, t->cur, t->rows[i], t->pos0 + i, scatter, wave);
        return;
    }
    for (int64_t r = b; r < e; r++) agg_radix_visit_view(t->c, v, t->cur, r, r, scatter, wave);
}
static void agg_radix_count_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t b, int64_t e) {
    (void)wid; agg_radix_run_rows(raw, v, b, e, false, false);
}
static void agg_radix_scatter_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t b, int64_t e) {
    (void)wid; agg_radix_run_rows(raw, v, b, e, true, false);
}
static void agg_radix_wave_scatter_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t b, int64_t e) {
    (void)wid; agg_radix_run_rows(raw, v, b, e, true, true);
}

/* The fused pass's body: rows [b, e) of a piece (each row its own
 * position), or the selected rows rows[b, e) at positions pos0 + i, staged
 * into the task's buffers. */
typedef struct {
    agg_radix_ctx_t* c;
    uint32_t*        cur;
    const int64_t*   rows;      /* sel mode: the run's decoded rows */
    int64_t          pos0;      /* sel mode: position of rows[0]; else 0 */
    int64_t          chunk0;    /* the chunk's first position */
    char*            stage;
    uint16_t*        spid;
} agg_radix_stage_t;
/* Rows [b, e) of a piece staged a field at a time (the type switches out
 * of the row loops), in blocks of at most RAY_ENC_VEC rows: each key into
 * its record slot and the running hash, then the partitions and counts,
 * the inputs at their widths, the positions.  The same records, ids and
 * counts as agg_radix_stage_view row by row. */
static void agg_radix_stage_block(agg_radix_stage_t* t, const agg_view_t* v, int64_t b, int64_t e) {
    agg_radix_ctx_t* c = t->c;
    const size_t rec = c->rec;
    const int64_t i0 = b - v->base;
    const int64_t n = e - b;
    char* out = t->stage + (size_t)(b - t->chunk0) * rec;
    uint16_t* spid = t->spid + (b - t->chunk0);
    uint64_t h[RAY_ENC_VEC];
    for (int64_t i = 0; i < n; i++) h[i] = 1469598103934665603ULL;
    for (uint32_t k = 0; k < c->n_keys; k++) {
        const void* kd = v->key_data[k];
        char* kp = out + (size_t)k * 8;
        #define STAGE_KEY(T) do { const T* src = (const T*)kd + i0;                    \
            for (int64_t i = 0; i < n; i++) {                                          \
                int64_t val = (int64_t)src[i];                                         \
                memcpy(kp + (size_t)i * rec, &val, 8);                                 \
                h[i] = (h[i] ^ (uint64_t)val) * 1099511628211ULL;                      \
            } } while (0)
        switch (c->key_cols[k]->type) {
            case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: STAGE_KEY(int64_t); break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: STAGE_KEY(int32_t); break;
            case RAY_I16: STAGE_KEY(int16_t); break;
            case RAY_U8: case RAY_BOOL: STAGE_KEY(uint8_t); break;
            case RAY_SYM:
                switch (v->key_attrs[k] & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8: STAGE_KEY(uint8_t); break;
                    case RAY_SYM_W16: STAGE_KEY(uint16_t); break;
                    case RAY_SYM_W32: STAGE_KEY(uint32_t); break;
                    default: STAGE_KEY(int64_t); break;
                }
                break;
            default: for (int64_t i = 0; i < n; i++) { memset(kp + (size_t)i * rec, 0, 8); h[i] *= 1099511628211ULL; }
        }
        #undef STAGE_KEY
    }
    const uint32_t mask = c->n_parts - 1;
    uint32_t* cur = t->cur;
    for (int64_t i = 0; i < n; i++) {
        uint32_t p = (uint32_t)(agg_radix_fmix64(h[i]) & mask);
        spid[i] = (uint16_t)p;
        cur[p]++;
    }
    #define STAGE_FIELD(src, off, ez) do {                                             \
        const char* sp = (const char*)(src) + (size_t)i0 * (ez);                       \
        char* dp = out + (off);                                                        \
        switch (ez) {                                                                  \
            case 1: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * rec, sp + i, 1); break;     \
            case 2: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * rec, sp + 2 * i, 2); break; \
            case 4: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * rec, sp + 4 * i, 4); break; \
            case 8: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * rec, sp + 8 * i, 8); break; \
            default: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * rec, sp + (size_t)i * (ez), (ez)); \
        } } while (0)
    for (uint32_t a = 0; a < c->n_aggs; a++) {
        if (v->val_data[a]) STAGE_FIELD(v->val_data[a], c->val_off[a], c->val_esz[a]);
        if (v->val2_data[a]) STAGE_FIELD(v->val2_data[a], c->val2_off[a], c->val2_esz[a]);
    }
    #undef STAGE_FIELD
    if (c->needs_row) {
        char* rp = out + c->row_off;
        for (int64_t i = 0; i < n; i++) { int64_t pos = b + i; memcpy(rp + (size_t)i * rec, &pos, 8); }
    }
}
static void agg_radix_stage_block_c(agg_radix_stage_t* t, const agg_view_t* v, int64_t b, int64_t e);
static void agg_radix_stage_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t b, int64_t e) {
    (void)wid;
    agg_radix_stage_t* t = raw;
    if (t->rows) {
        for (int64_t i = b; i < e; i++)
            agg_radix_stage_view(t->c, v, t->cur, t->stage, t->spid, t->chunk0, t->rows[i], t->pos0 + i);
        return;
    }
    if (t->c->compact) {
        for (int64_t r = b; r < e; r += RAY_ENC_VEC)
            agg_radix_stage_block_c(t, v, r, e - r < RAY_ENC_VEC ? e : r + RAY_ENC_VEC);
        return;
    }
    for (int64_t r = b; r < e; r += RAY_ENC_VEC)
        agg_radix_stage_block(t, v, r, e - r < RAY_ENC_VEC ? e : r + RAY_ENC_VEC);
}

/* Copy n records of `rec` bytes from the staging buffer to their slots:
 * the chunk's partition runs, starting at wcur (offsets inside the chunk's
 * record range at dst).  Fixed record sizes copy with constant widths. */
static void agg_radix_stage_scatter(char* dst, const char* stage, const uint16_t* spid,
                                    uint32_t* wcur, int64_t n, size_t rec) {
    #define STAGE_COPY(W) for (int64_t i = 0; i < n; i++) \
        memcpy(dst + (size_t)wcur[spid[i]]++ * (W), stage + (size_t)i * (W), (W))
    switch (rec) {
        case 16: STAGE_COPY(16); break;
        case 24: STAGE_COPY(24); break;
        case 32: STAGE_COPY(32); break;
        case 40: STAGE_COPY(40); break;
        case 48: STAGE_COPY(48); break;
        default:                                /* records are 8-aligned: word copies, no call */
            for (int64_t i = 0; i < n; i++) {
                char* d = dst + (size_t)wcur[spid[i]]++ * rec;
                const char* s = stage + (size_t)i * rec;
                for (size_t w = 0; w < rec; w += 8) memcpy(d + w, s + w, 8);
            }
            break;
    }
    #undef STAGE_COPY
}

/* agg_radix_stage_block of a compact context: rows [b, e) of a piece,
 * hashed over every key (plain ones read in place through the view), the
 * staged sources' fields copied into the staged rows (srec bytes each),
 * the partitions and counts recorded.  The plain fields and the positions
 * are left to agg_radix_scatter_compact. */
static void agg_radix_stage_block_c(agg_radix_stage_t* t, const agg_view_t* v, int64_t b, int64_t e) {
    agg_radix_ctx_t* c = t->c;
    const size_t srec = c->srec;
    const int64_t i0 = b - v->base;
    const int64_t n = e - b;
    char* out = t->stage + (size_t)(b - t->chunk0) * srec;
    uint16_t* spid = t->spid + (b - t->chunk0);
    uint64_t h[RAY_ENC_VEC];
    for (int64_t i = 0; i < n; i++) h[i] = 1469598103934665603ULL;
    for (uint32_t k = 0; k < c->n_keys; k++) {
        const void* kd = v->key_data[k];
        const agg_radix_cf_t* f = &c->cf[k];
        char* kp = out + f->soff;
        if (f->kind == AGG_CF_STAGED) {
            #define STAGE_KEY(T) do { const T* src = (const T*)kd + i0;                \
                for (int64_t i = 0; i < n; i++) {                                      \
                    int64_t val = (int64_t)src[i];                                     \
                    memcpy(kp + (size_t)i * srec, &val, 8);                            \
                    h[i] = (h[i] ^ (uint64_t)val) * 1099511628211ULL;                  \
                } } while (0)
            switch (c->key_cols[k]->type) {
                case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: STAGE_KEY(int64_t); break;
                case RAY_I32: case RAY_DATE: case RAY_TIME: STAGE_KEY(int32_t); break;
                case RAY_I16: STAGE_KEY(int16_t); break;
                case RAY_U8: case RAY_BOOL: STAGE_KEY(uint8_t); break;
                case RAY_SYM:
                    switch (v->key_attrs[k] & RAY_SYM_W_MASK) {
                        case RAY_SYM_W8: STAGE_KEY(uint8_t); break;
                        case RAY_SYM_W16: STAGE_KEY(uint16_t); break;
                        case RAY_SYM_W32: STAGE_KEY(uint32_t); break;
                        default: STAGE_KEY(int64_t); break;
                    }
                    break;
                default: for (int64_t i = 0; i < n; i++) { memset(kp + (size_t)i * srec, 0, 8); h[i] *= 1099511628211ULL; }
            }
            #undef STAGE_KEY
            continue;
        }
        #define HASH_KEY(T) do { const T* src = (const T*)kd + i0;                     \
            for (int64_t i = 0; i < n; i++)                                            \
                h[i] = (h[i] ^ (uint64_t)(int64_t)src[i]) * 1099511628211ULL; } while (0)
        switch (f->kind) {
            case AGG_CF_KEY_I64: HASH_KEY(int64_t); break;
            case AGG_CF_KEY_I32: HASH_KEY(int32_t); break;
            case AGG_CF_KEY_I16: HASH_KEY(int16_t); break;
            case AGG_CF_KEY_U8:  HASH_KEY(uint8_t); break;
            case AGG_CF_KEY_U16: HASH_KEY(uint16_t); break;
            case AGG_CF_KEY_U32: HASH_KEY(uint32_t); break;
            default: for (int64_t i = 0; i < n; i++) h[i] *= 1099511628211ULL;
        }
        #undef HASH_KEY
    }
    const uint32_t mask = c->n_parts - 1;
    uint32_t* cur = t->cur;
    for (int64_t i = 0; i < n; i++) {
        uint32_t p = (uint32_t)(agg_radix_fmix64(h[i]) & mask);
        spid[i] = (uint16_t)p;
        cur[p]++;
    }
    /* the staged inputs: each field names its aggregate and side */
    for (uint32_t fi = c->n_keys; fi < c->n_cf; fi++) {
        const agg_radix_cf_t* f = &c->cf[fi];
        if (f->kind != AGG_CF_STAGED) continue;
        const void* vd = f->y ? v->val2_data[f->agg] : v->val_data[f->agg];
        const char* sp = (const char*)vd + (size_t)i0 * f->w;
        char* dp = out + f->soff;
        switch (f->w) {
            case 1: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * srec, sp + i, 1); break;
            case 2: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * srec, sp + 2 * i, 2); break;
            case 4: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * srec, sp + 4 * i, 4); break;
            case 8: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * srec, sp + 8 * i, 8); break;
            default: for (int64_t i = 0; i < n; i++) memcpy(dp + (size_t)i * srec, sp + (size_t)i * f->w, f->w);
        }
    }
}

/* The scatter of a compact chunk: n staged rows (positions pos0 ..) to
 * their slots at wcur (offsets inside the chunk's record range at dst),
 * each record built as the two passes' scatter builds it: the staged
 * fields from the staged row, the plain ones read in place at the row,
 * the position. */
static void agg_radix_scatter_compact(const agg_radix_ctx_t* c, char* dst, const char* stage,
                                      const uint16_t* spid, uint32_t* wcur, int64_t pos0, int64_t n) {
    const size_t rec = c->rec, srec = c->srec;
    const uint32_t nf = c->n_cf;
    const agg_radix_cf_t* cf = c->cf;
    const size_t row_off = c->row_off;
    const bool needs_row = c->needs_row;
    for (int64_t i = 0; i < n; i++) {
        char* d = dst + (size_t)wcur[spid[i]]++ * rec;
        const char* sr = stage + (size_t)i * srec;
        const int64_t r = pos0 + i;
        for (uint32_t fi = 0; fi < nf; fi++) {
            const agg_radix_cf_t* f = &cf[fi];
            int64_t kv;
            switch (f->kind) {
                case AGG_CF_STAGED:
                    switch (f->w) {
                        case 1: memcpy(d + f->off, sr + f->soff, 1); break;
                        case 2: memcpy(d + f->off, sr + f->soff, 2); break;
                        case 4: memcpy(d + f->off, sr + f->soff, 4); break;
                        case 8: memcpy(d + f->off, sr + f->soff, 8); break;
                        default: memcpy(d + f->off, sr + f->soff, f->w);
                    }
                    continue;
                case AGG_CF_KEY_I64: kv = ((const int64_t*)f->plain)[r]; break;
                case AGG_CF_KEY_I32: kv = ((const int32_t*)f->plain)[r]; break;
                case AGG_CF_KEY_I16: kv = ((const int16_t*)f->plain)[r]; break;
                case AGG_CF_KEY_U8:  kv = ((const uint8_t*)f->plain)[r]; break;
                case AGG_CF_KEY_U16: kv = ((const uint16_t*)f->plain)[r]; break;
                case AGG_CF_KEY_U32: kv = ((const uint32_t*)f->plain)[r]; break;
                default:
                    memcpy(d + f->off, f->plain + (size_t)r * f->esz, f->esz);
                    continue;
            }
            memcpy(d + f->off, &kv, 8);
        }
        if (needs_row) memcpy(d + row_off, &r, sizeof r);
    }
}

/* The staged form of one pass over chunk ch (out of line: the plain loops
 * below stay as compact as they were): pieces of the chunk's rows (R-WALK),
 * or runs of its selected rows (R-SPARSE), through the task's cursors cs
 * (the chunks of one task are consecutive); a range error is noted keyed
 * by the chunk's first position.  A wave's scatter (`wave`) writes only the
 * records of its partitions. */
static void agg_radix_chunk_staged(agg_radix_ctx_t* c, int64_t ch, uint32_t wid, bool scatter, bool wave,
                                   agg_curs_t* cs) {
    int64_t start = ch * c->chunk_rows;
    int64_t end = start + c->chunk_rows;
    if (end > c->n_in) end = c->n_in;
    uint32_t* cur = c->cnt + (size_t)ch * c->n_parts;
    if (!scatter) memset(cur, 0, (size_t)c->n_parts * sizeof(uint32_t));
    const agg_rr_t* rr = scatter ? c->rr : c->count_rr;   /* pass 1 hashes the keys only */
    agg_body_fn body = !scatter ? agg_radix_count_rows
                     : wave ? agg_radix_wave_scatter_rows : agg_radix_scatter_rows;
    if (!c->sel) {
        agg_radix_run_t t = { c, cur, NULL, 0 };
        (void)agg_walk_cs(rr, cs, &t, body, wid, start, end, start);
        return;
    }
    if (ray_scan_fail_skip(rr->fail, start)) return;
    int64_t rows[AGG_SEL_CHUNK];
    agg_sel_cursor_t sc;
    agg_sel_cursor_init(&sc, c->sel, c->sel_prefix, start, end);
    int64_t pos = start, cn;
    while ((cn = agg_sel_cursor_next(&sc, rows)) > 0) {
        agg_radix_run_t t = { c, cur, rows, pos };
        ray_t* err = agg_idx_walk(rr, cs, rows, 0, cn, &t, body, wid);
        if (err) { ray_scan_fail_note(rr->fail, start, err); break; }
        pos += cn;
    }
}

/* Run one pass over chunk `ch` of a plain context (whole columns): input
 * positions [start,end) in chunk order (selected-row ordinals in sel mode,
 * decoded to original rows).  RAY_INLINE and `scatter` constant at every
 * call site, so the count and the scatter loops specialize.  A chunk's
 * count row doubles as its pass-2 write cursors and is touched only by the
 * task running that chunk.  A staged context runs agg_radix_chunk_staged. */
RAY_INLINE void agg_radix_chunk_pass(agg_radix_ctx_t* c, int64_t ch, bool scatter, bool wave) {
    int64_t start = ch * c->chunk_rows;
    int64_t end = start + c->chunk_rows;
    if (end > c->n_in) end = c->n_in;
    uint32_t* cur = c->cnt + (size_t)ch * c->n_parts;
    if (!scatter) memset(cur, 0, (size_t)c->n_parts * sizeof(uint32_t));
    if (c->sel) {
        int64_t rows[AGG_SEL_CHUNK];
        agg_sel_cursor_t sc;
        agg_sel_cursor_init(&sc, c->sel, c->sel_prefix, start, end);
        int64_t pos = start;
        int64_t cn;
        while ((cn = agg_sel_cursor_next(&sc, rows)) > 0)
            for (int64_t i = 0; i < cn; i++)
                agg_radix_visit(c, cur, rows[i], pos++, scatter, wave);
        return;
    }
    /* Keep the next rows requested from storage, block by block (scan
     * read-ahead): the count pass is the first read of the key columns, the
     * scatter pass the first of the aggregate columns. */
    for (int64_t b = start; b < end; b += 8192) {
        int64_t e = end - b > 8192 ? b + 8192 : end;
        ray_pool_scan_at(c->n_in, b, end);
        for (int64_t r = b; r < e; r++)
            agg_radix_visit(c, cur, r, r, scatter, wave);
    }
}

/* One cursor set per task call (staged sources), over its consecutive chunks;
 * on OOM the first chunk's failure is noted and the call does nothing. */
static bool agg_radix_task_curs(agg_radix_ctx_t* c, int64_t ch0, agg_curs_t* cs) {
    cs->cur = NULL;
    if (!c->rr->staged || agg_curs_init(cs, c->rr)) return true;
    ray_scan_fail_note(c->rr->fail, ch0 * c->chunk_rows, ray_error("oom", NULL));
    return false;
}

/* One task call of a pass over its consecutive chunks [start,end).  One
 * body, two instances per pass (`staged` constant): the plain instance is
 * the whole-column chunk loop and never builds or touches a cursor set, so
 * its codegen is the pre-range-read one; the staged instance holds one
 * cursor set across its chunks.  The dispatch picks the instance from the
 * context (agg_radix_count_fn or _count_staged_fn, and likewise for the
 * scatter). */
RAY_INLINE void agg_radix_pass_body(agg_radix_ctx_t* c, uint32_t wid, int64_t start, int64_t end,
                                    bool scatter, bool wave, const bool staged) {
    if (!staged) {
        for (int64_t ch = start; ch < end; ch++)
            agg_radix_chunk_pass(c, ch, scatter, wave);
        return;
    }
    agg_curs_t cs;
    if (!agg_radix_task_curs(c, start, &cs)) return;
    for (int64_t ch = start; ch < end; ch++)
        agg_radix_chunk_staged(c, ch, wid, scatter, wave, &cs);
    agg_curs_free(&cs, c->rr);
}

/* Phase 1, pass 1: count each chunk's rows per partition. */
static void agg_radix_count_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_pass_body((agg_radix_ctx_t*)vctx, wid, start, end, false, false, false);
}
static void agg_radix_count_staged_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_pass_body((agg_radix_ctx_t*)vctx, wid, start, end, false, false, true);
}

/* Phase 1, prefix: for a band of partitions, turn every chunk's count into
 * its offset inside the partition (exclusive scan down the chunks, so chunk
 * order — ascending input order — is the slot order) and store the
 * partition's total in part_start[p + 1] for the caller's scan. */
static void agg_radix_prefix_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    for (int64_t band = start; band < end; band++) {
        uint32_t p0 = (uint32_t)band * AGG_RADIX_PREFIX_BAND;
        uint32_t np = c->n_parts - p0;
        if (np > AGG_RADIX_PREFIX_BAND) np = AGG_RADIX_PREFIX_BAND;
        uint64_t run[AGG_RADIX_PREFIX_BAND] = {0};
        for (uint32_t ch = 0; ch < c->n_chunks; ch++) {
            uint32_t* row = c->cnt + (size_t)ch * c->n_parts + p0;
            for (uint32_t j = 0; j < np; j++) {
                uint32_t n = row[j];
                row[j] = (uint32_t)run[j];
                run[j] += n;
            }
        }
        for (uint32_t j = 0; j < np; j++) {
            /* Offsets are uint32 and phase 2 indexes a partition's rows with
             * 32-bit gids: a partition past that bound takes the fallback. */
            if (run[j] > UINT32_MAX)
                atomic_store_explicit(&c->part_overflow, 1, memory_order_relaxed);
            c->part_start[p0 + j + 1] = (int64_t)run[j];
        }
    }
}

/* Phase 1, pass 2: write each chunk's packed records at their exact slots. */
static void agg_radix_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_pass_body((agg_radix_ctx_t*)vctx, wid, start, end, true, false, false);
}
static void agg_radix_scatter_staged_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_pass_body((agg_radix_ctx_t*)vctx, wid, start, end, true, false, true);
}

/* Pass 2 of one wave: the same walk, writing only the records of the
 * partitions [wave_lo, wave_hi).  Each partition belongs to one wave, so its
 * chunk cursors advance exactly as in the one-pass scatter and its records
 * land in the same order. */
static void agg_radix_wave_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_pass_body((agg_radix_ctx_t*)vctx, wid, start, end, true, true, false);
}
static void agg_radix_wave_scatter_staged_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_pass_body((agg_radix_ctx_t*)vctx, wid, start, end, true, true, true);
}

/* The fused pass over chunk ch (a staged context, chunk-major payload):
 * the chunk is read once (pieces, or runs of its selected rows) into the
 * task's staging buffers, its count row becomes the chunk-local exclusive
 * prefix (each partition's offset inside the chunk's record range), and
 * the staged records are scattered there.  A range error is noted keyed by
 * the chunk's first position, and the chunk is not scattered. */
static void agg_radix_chunk_fused(agg_radix_ctx_t* c, int64_t ch, uint32_t wid, agg_curs_t* cs,
                                  char* stage, uint16_t* spid, uint32_t* wcur) {
    const agg_rr_t* rr = c->rr;
    int64_t start = ch * c->chunk_rows;
    int64_t end = start + c->chunk_rows;
    if (end > c->n_in) end = c->n_in;
    uint32_t* cur = c->cnt + (size_t)ch * c->n_parts;
    memset(cur, 0, (size_t)c->n_parts * sizeof(uint32_t));
    agg_radix_stage_t t = { c, cur, NULL, 0, start, stage, spid };
    if (!c->sel) {
        if (!agg_walk_cs(rr, cs, &t, agg_radix_stage_rows, wid, start, end, start)) return;
    } else {
        if (ray_scan_fail_skip(rr->fail, start)) return;
        int64_t rows[AGG_SEL_CHUNK];
        agg_sel_cursor_t sc;
        agg_sel_cursor_init(&sc, c->sel, c->sel_prefix, start, end);
        int64_t pos = start, cn;
        while ((cn = agg_sel_cursor_next(&sc, rows)) > 0) {
            t.rows = rows; t.pos0 = pos;
            ray_t* err = agg_idx_walk(rr, cs, rows, 0, cn, &t, agg_radix_stage_rows, wid);
            if (err) { ray_scan_fail_note(rr->fail, start, err); return; }
            pos += cn;
        }
    }
    uint32_t run = 0;
    for (uint32_t p = 0; p < c->n_parts; p++) {
        uint32_t n = cur[p];
        cur[p] = wcur[p] = run;
        run += n;
    }
    if (c->compact)
        agg_radix_scatter_compact(c, c->pay + (size_t)start * c->rec, stage, spid, wcur, start, end - start);
    else
        agg_radix_stage_scatter(c->pay + (size_t)start * c->rec, stage, spid, wcur, end - start, c->rec);
}

/* Phase 1 of a fused context: one task call's consecutive chunks (a few
 * tasks per worker, so a vector a chunk boundary cuts is decoded once per
 * task, not per chunk) through one cursor set and one set of staging
 * buffers (a chunk's records, its
 * partition ids, the partitions' write cursors), allocated by the call
 * and freed by it; on OOM the first chunk's failure is noted. */
static void agg_radix_fused_fn(void* vctx, uint32_t wid, int64_t g0, int64_t g1) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    /* tasks g0..g1 of fused_tasks, each a run of consecutive chunks */
    int64_t start = (int64_t)((uint64_t)g0 * c->n_chunks / c->fused_tasks);
    int64_t end = (int64_t)((uint64_t)g1 * c->n_chunks / c->fused_tasks);
    if (start >= end) return;
    agg_curs_t cs;
    if (!agg_radix_task_curs(c, start, &cs)) return;
    char* stage = ray_alloc_raw((size_t)c->chunk_rows * (c->compact ? (c->srec ? c->srec : 1) : c->rec));
    uint16_t* spid = ray_alloc_raw((size_t)c->chunk_rows * sizeof(uint16_t));
    uint32_t* wcur = ray_alloc_raw((size_t)c->n_parts * sizeof(uint32_t));
    if (!stage || !spid || !wcur)
        ray_scan_fail_note(c->rr->fail, start * c->chunk_rows, ray_error("oom", NULL));
    else
        for (int64_t ch = start; ch < end; ch++)
            agg_radix_chunk_fused(c, ch, wid, &cs, stage, spid, wcur);
    ray_free_raw(stage); ray_free_raw(spid); ray_free_raw(wcur);
    agg_curs_free(&cs, c->rr);
}

/* A fused context's partition totals, for a band of partitions: the sum of
 * every chunk's run length (the next partition's offset, or the chunk's
 * row count for the last), stored in part_start[p + 1] for the caller's
 * scan (as agg_radix_prefix_fn stores them); each run's offset and length
 * also go to the partition's row of runs, which phase 2 reads in order. */
static void agg_radix_fused_totals_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    for (int64_t band = start; band < end; band++) {
        uint32_t p0 = (uint32_t)band * AGG_RADIX_PREFIX_BAND;
        uint32_t np = c->n_parts - p0;
        if (np > AGG_RADIX_PREFIX_BAND) np = AGG_RADIX_PREFIX_BAND;
        uint64_t run[AGG_RADIX_PREFIX_BAND] = {0};
        for (uint32_t ch = 0; ch < c->n_chunks; ch++) {
            const uint32_t* row = c->cnt + (size_t)ch * c->n_parts;
            int64_t cs = (int64_t)ch * c->chunk_rows;
            int64_t clen = (cs + c->chunk_rows < c->n_in ? cs + c->chunk_rows : c->n_in) - cs;
            for (uint32_t j = 0; j < np; j++) {
                uint32_t p = p0 + j;
                uint64_t next = p + 1 < c->n_parts ? row[p + 1] : (uint64_t)clen;
                uint32_t* rp = c->runs + ((size_t)p * c->n_chunks + ch) * 2;
                rp[0] = row[p];
                rp[1] = (uint32_t)(next - row[p]);
                run[j] += next - row[p];
            }
        }
        for (uint32_t j = 0; j < np; j++) {
            if (run[j] > UINT32_MAX)
                atomic_store_explicit(&c->part_overflow, 1, memory_order_relaxed);
            c->part_start[p0 + j + 1] = (int64_t)run[j];
        }
    }
}

/* Reduce a finished partition to the groups a top-N emit can select.
 *
 * agg_radix_select_topn keeps every group strictly beyond the threshold T
 * (the N-th value, N = take) and, of the groups AT T, the N with the
 * smallest first row; it emits them in first-row order.  Rank the groups by
 * (value in the keep direction, then smaller first row) — a total order,
 * first rows being distinct.  The kept set is a prefix of that order no
 * longer than 2N - 1: fewer than N groups lie strictly beyond T, then come
 * the tied ones by first row.  Every group of that prefix ranks below 2N
 * inside its own partition too (only prefix members rank ahead of it), so a
 * partition that keeps its 2N best (cmp_cap) loses no kept group, and the
 * union of the survivors still holds the N best values, so the selection
 * over the survivors finds the same T, the same strict groups and the same
 * tied ones: the emitted rows are byte-identical.  States are copied as they
 * are (streaming accumulators, no destroy hook) and so are their keys and
 * first rows.  On an allocation failure the partition keeps every group,
 * which is the unreduced, equally valid input of the same selection. */
static void agg_radix_part_compact(agg_radix_ctx_t* c, char** states,
                                   int64_t** first_row, int64_t** gkeys, int64_t* ngp) {
    int64_t ng = *ngp, cap = c->cmp_cap;
    if (cap <= 0 || ng <= cap) return;
    uint32_t n_keys = c->n_keys;
    size_t kw = (size_t)(n_keys ? n_keys : 1);
    double*  vals = ray_alloc_raw((size_t)ng * sizeof(double));
    int64_t* heap = ray_alloc_raw((size_t)cap * sizeof(int64_t));
    if (!vals || !heap ||
        !agg_group_values_f64(c->cmp_vt, *states, c->block, c->cmp_off, NULL, ng,
                              c->cmp_param, vals)) {
        ray_free_raw(vals); ray_free_raw(heap);
        return;
    }
    const int64_t* fr = *first_row;
    uint8_t desc = c->cmp_desc;
    /* i ranks after j: worse value, or the same value seen later. */
    #define CMP_AFTER(i, j) ((desc ? vals[i] < vals[j] : vals[i] > vals[j]) || \
                             (vals[i] == vals[j] && fr[i] > fr[j]))
    int64_t hn = 0;
    for (int64_t gg = 0; gg < ng; gg++) {        /* root = the last kept */
        if (hn < cap) {
            int64_t j = hn++;
            heap[j] = gg;
            while (j > 0) {
                int64_t par = (j - 1) / 2;
                if (!CMP_AFTER(heap[j], heap[par])) break;
                int64_t t = heap[par]; heap[par] = heap[j]; heap[j] = t; j = par;
            }
        } else if (CMP_AFTER(heap[0], gg)) {
            heap[0] = gg;
            for (int64_t j = 0;;) {
                int64_t l = 2 * j + 1, r = l + 1, m = j;
                if (l < cap && CMP_AFTER(heap[l], heap[m])) m = l;
                if (r < cap && CMP_AFTER(heap[r], heap[m])) m = r;
                if (m == j) break;
                int64_t t = heap[m]; heap[m] = heap[j]; heap[j] = t; j = m;
            }
        }
    }
    #undef CMP_AFTER
    ray_free_raw(vals);
    char*    ns = ray_alloc_raw((size_t)cap * c->block);
    int64_t* nk = ray_alloc_raw((size_t)cap * kw * sizeof(int64_t));
    int64_t* nf = ray_alloc_raw((size_t)cap * sizeof(int64_t));
    if (!ns || !nk || !nf) {
        ray_free_raw(ns); ray_free_raw(nk); ray_free_raw(nf); ray_free_raw(heap);
        return;
    }
    for (int64_t i = 0; i < cap; i++) {
        int64_t gg = heap[i];
        memcpy(ns + (size_t)i * c->block, *states + (size_t)gg * c->block, c->block);
        memcpy(nk + (size_t)i * kw, *gkeys + (size_t)gg * kw, kw * sizeof(int64_t));
        nf[i] = fr[gg];
    }
    ray_free_raw(heap);
    ray_free_raw(*states); ray_free_raw(*gkeys); ray_free_raw(*first_row);
    *states = ns; *gkeys = nk; *first_row = nf; *ngp = cap;
    atomic_fetch_add_explicit(&c->cmp_parts, 1, memory_order_relaxed);
}

#if AGG_RADIX_FILE_SPILL
static bool agg_spill_pwrite(int fd, const char* p, size_t n, int64_t off) {
    while (n) {
        ssize_t w = pwrite(fd, p, n, (off_t)off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w; off += w; n -= (size_t)w;
    }
    return true;
}
static bool agg_spill_pread(int fd, char* p, size_t n, int64_t off) {
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; off += r; n -= (size_t)r;
    }
    return true;
}
#endif

/* The spill file: unlinked as soon as it is open, in the heap's spill
 * directory (RAY_HEAP_SWAP, else TMPDIR, else /tmp — as heap_swap_dir). */
static int agg_radix_spill_open(void) {
#if AGG_RADIX_FILE_SPILL
    static _Atomic(uint64_t) ctr = 0;
    const char* d = getenv("RAY_HEAP_SWAP");
    if (!(d && *d)) d = getenv("TMPDIR");
    if (!(d && *d) || strlen(d) > 400) d = "/tmp";
    size_t dl = strlen(d);
    char path[512];
    snprintf(path, sizeof path, "%s%sraygroup_%d_%llu.dat", d, d[dl - 1] == '/' ? "" : "/",
             (int)getpid(),
             (unsigned long long)atomic_fetch_add_explicit(&ctr, 1, memory_order_relaxed));
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd >= 0) unlink(path);
    return fd;
#else
    return -1;
#endif
}

/* Append a full (or the last) buffer of one wave as one block. */
static void agg_radix_spill_flush(agg_radix_spill_t* s, agg_radix_sbuf_t* b, uint32_t wave) {
    if (!b->n) return;
#if AGG_RADIX_FILE_SPILL
    size_t rb = (size_t)b->n * s->rec, sb = (size_t)b->n * sizeof(uint32_t);
    int64_t bi = atomic_fetch_add_explicit(&s->n_blks, 1, memory_order_relaxed);
    int64_t off = atomic_fetch_add_explicit(&s->tail, (int64_t)(rb + sb), memory_order_relaxed);
    if (bi >= s->blk_cap || ray_agg_radix_spill == 4 ||
        !agg_spill_pwrite(s->fd, b->buf, rb, off) ||
        !agg_spill_pwrite(s->fd, b->buf + (size_t)s->bcap * s->rec, sb, off + (int64_t)rb))
        atomic_store_explicit(&s->err, 1, memory_order_relaxed);
    else
        s->blks[bi] = (agg_radix_blk_t){ off, b->n, wave };
#else
    (void)wave;
    atomic_store_explicit(&s->err, 1, memory_order_relaxed);
#endif
    b->n = 0;
}

/* Where a spilled run scatters input position pos: wave 0 to its payload
 * slot, a later wave to the next record of this worker's buffer, with the
 * slot it will take when read back stored beside it (a full buffer is
 * flushed first).  The chunk cursors advance for every partition exactly as
 * in the one-pass scatter, so every record's slot is the one-pass slot less
 * its wave base. */
RAY_INLINE char* agg_radix_spill_dst(agg_radix_ctx_t* c, uint32_t* cur,
                                     agg_radix_sbuf_t* wb, int64_t pos) {
    agg_radix_spill_t* s = c->spill;
    uint32_t p = c->pid[pos];
    uint32_t w = c->wave_of[p];
    int64_t slot = c->part_start[p] - s->wave_base[w] + cur[p]++;
    if (w == 0) return c->pay + (size_t)slot * c->rec;
    agg_radix_sbuf_t* b = &wb[w];
    if (b->n == s->bcap) agg_radix_spill_flush(s, b, w);
    ((uint32_t*)(void*)(b->buf + (size_t)s->bcap * c->rec))[b->n] = (uint32_t)slot;
    return b->buf + (size_t)b->n++ * c->rec;
}

/* The spilled scatter of a plain context (whole columns). */
static void agg_radix_spill_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    agg_radix_sbuf_t* wb = c->spill->bufs + (size_t)wid * c->spill->n_waves;
    for (int64_t ch = start; ch < end; ch++) {
        int64_t cs = ch * c->chunk_rows, ce = cs + c->chunk_rows;
        if (ce > c->n_in) ce = c->n_in;
        uint32_t* cur = c->cnt + (size_t)ch * c->n_parts;
        if (c->sel) {
            int64_t rows[AGG_SEL_CHUNK];
            agg_sel_cursor_t sc;
            agg_sel_cursor_init(&sc, c->sel, c->sel_prefix, cs, ce);
            int64_t pos = cs, cn;
            while ((cn = agg_sel_cursor_next(&sc, rows)) > 0)
                for (int64_t i = 0; i < cn; i++, pos++)
                    agg_radix_put(c, agg_radix_spill_dst(c, cur, wb, pos), rows[i], pos);
            continue;
        }
        for (int64_t b = cs; b < ce; b += 8192) {
            int64_t e = ce - b > 8192 ? b + 8192 : ce;
            ray_pool_scan_at(c->n_in, b, ce);
            for (int64_t r = b; r < e; r++)
                agg_radix_put(c, agg_radix_spill_dst(c, cur, wb, r), r, r);
        }
    }
}

/* The spilled scatter of a staged context: rows read through views, as
 * agg_radix_chunk_staged reads them; a range error is noted keyed by the
 * chunk's first position and leaves the chunk unscattered. */
typedef struct {
    agg_radix_ctx_t*  c;
    uint32_t*         cur;
    const int64_t*    rows;      /* sel mode: the chunk's decoded rows */
    int64_t           pos0;
    agg_radix_sbuf_t* wb;
} agg_radix_spill_rows_t;
static void agg_radix_spill_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t b, int64_t e) {
    (void)wid;
    agg_radix_spill_rows_t* t = raw;
    if (t->rows) {
        for (int64_t i = b; i < e; i++) {
            int64_t pos = t->pos0 + i;
            agg_radix_put_view(t->c, v, agg_radix_spill_dst(t->c, t->cur, t->wb, pos), t->rows[i], pos);
        }
        return;
    }
    for (int64_t r = b; r < e; r++)
        agg_radix_put_view(t->c, v, agg_radix_spill_dst(t->c, t->cur, t->wb, r), r, r);
}
static void agg_radix_spill_scatter_staged_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    const agg_rr_t* rr = c->rr;
    agg_radix_sbuf_t* wb = c->spill->bufs + (size_t)wid * c->spill->n_waves;
    agg_curs_t curs;
    if (!agg_radix_task_curs(c, start, &curs)) return;
    for (int64_t ch = start; ch < end; ch++) {
        int64_t cs = ch * c->chunk_rows, ce = cs + c->chunk_rows;
        if (ce > c->n_in) ce = c->n_in;
        uint32_t* cur = c->cnt + (size_t)ch * c->n_parts;
        if (!c->sel) {
            agg_radix_spill_rows_t t = { c, cur, NULL, 0, wb };
            (void)agg_walk_cs(rr, &curs, &t, agg_radix_spill_rows, wid, cs, ce, cs);
            continue;
        }
        if (ray_scan_fail_skip(rr->fail, cs)) continue;
        int64_t rows[AGG_SEL_CHUNK];
        agg_sel_cursor_t sc;
        agg_sel_cursor_init(&sc, c->sel, c->sel_prefix, cs, ce);
        int64_t pos = cs, cn;
        while ((cn = agg_sel_cursor_next(&sc, rows)) > 0) {
            agg_radix_spill_rows_t t = { c, cur, rows, pos, wb };
            ray_t* err = agg_idx_walk(rr, &curs, rows, 0, cn, &t, agg_radix_spill_rows, wid);
            if (err) { ray_scan_fail_note(rr->fail, cs, err); break; }
            pos += cn;
        }
    }
    agg_curs_free(&curs, rr);
}

/* Read the blocks of one wave back into their payload slots. */
static void agg_radix_readback_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    agg_radix_spill_t* s = c->spill;
#if AGG_RADIX_FILE_SPILL
    char* rb = s->rbuf + (size_t)wid * s->rstride;
    for (int64_t i = start; i < end; i++) {
        const agg_radix_blk_t* bk = &s->blks[s->wave_blks[i]];
        size_t rbytes = (size_t)bk->n * c->rec;
        if (ray_agg_radix_spill == 5)   /* test fault: the pool is cancelled mid read-back */
            atomic_store_explicit(&ray_pool_get()->cancelled, 1, memory_order_relaxed);
        if (ray_agg_radix_spill == 3 ||
            !agg_spill_pread(s->fd, rb, rbytes + (size_t)bk->n * sizeof(uint32_t), bk->off)) {
            atomic_store_explicit(&s->err, 1, memory_order_relaxed);
            continue;
        }
        const uint32_t* slots = (const uint32_t*)(const void*)(rb + rbytes);
        for (uint32_t j = 0; j < bk->n; j++)
            memcpy(c->pay + (size_t)slots[j] * c->rec, rb + (size_t)j * c->rec, c->rec);
    }
#else
    (void)wid; (void)start; (void)end;
    atomic_store_explicit(&s->err, 1, memory_order_relaxed);
#endif
}

/* Phase 2: group + accumulate one partition by walking its packed payload
 * records SEQUENTIALLY.  Keys here are disjoint from every other partition, so
 * a partition-local open-addressing hash suffices.  Hash/equality compare the
 * contiguous packed keys at the head of each record (no scattered key-column
 * re-reads), and each agg's dense value buffer is gathered straight from the
 * records in the same sequential pass.  Keys are disjoint from other
 * partitions so the partition-local open-addressing hash suffices.  Under a
 * top-N emit (cmp_cap) the finished partition is cut to its candidates
 * (agg_radix_part_compact) before it is published. */
static void agg_radix_group_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    size_t key_bytes = (size_t)n_keys * 8;
    for (int64_t pi = start; pi < end; pi++) {
        uint32_t p = c->part_lo + (uint32_t)pi;
        agg_radix_part_t* pr = &c->parts[p];
        if (ray_agg_radix_spill == 6 && c->pay_base > 0)   /* test fault: cancelled in a later wave */
            atomic_store_explicit(&ray_pool_get()->cancelled, 1, memory_order_relaxed);

        /* Rows hashing to p: its contiguous slot range, in input order. */
        int64_t total = c->part_start[p + 1] - c->part_start[p];
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
        {
            const char* rec = c->pay + (size_t)(c->part_start[p] - c->pay_base) * c->rec;
            for (int64_t i = 0; i < total; i++, rec += c->rec) {
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
        if (c->cmp_cap > 0) agg_radix_part_compact(c, &states, &first_row, &gkeys, &ng);
        pr->states = states; pr->first_row = first_row; pr->keys = gkeys; pr->ng = ng;
        continue;
    oom:
        ray_free_raw(ht); ray_free_raw(ht_salt); ray_free_raw(first_row); ray_free_raw(states); ray_free_raw(keyp); ray_free_raw(gid); ray_free_raw(gkeys);
        for (uint32_t a = 0; a < n_aggs; a++) { ray_free_raw(gv[a]); ray_free_raw(gy[a]); }
        ray_free_raw(gv); ray_free_raw(gy);
        pr->oom = 1; return;
    }
}

/* Phase 2 of a fused context: agg_radix_group_fn over each partition's
 * chunk runs in chunk order (the same records in the same order as the
 * global layout).  A copy rather than a shared body: one always-inline
 * body over a run table (the global layout one run per partition)
 * measured q16 1.3% slower on generation 0 (73.8 against 72.8 ms) and 3%
 * slower on generation 2.  The two must group the same records in the
 * same order: scanrange/v2_radix_record_order compares their answers
 * byte for byte.  Under a top-N emit (cmp_cap) a finished partition is cut to
 * its candidates, as agg_radix_group_fn cuts it. */
static void agg_radix_group_fused_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    size_t key_bytes = (size_t)n_keys * 8;
    for (int64_t pi = start; pi < end; pi++) {
        uint32_t p = (uint32_t)pi;
        agg_radix_part_t* pr = &c->parts[p];

        /* Rows hashing to p: its contiguous slot range, in input order. */
        int64_t total = c->part_start[p + 1] - c->part_start[p];
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
        /* The records of p: one run per chunk, in chunk order. */
        for (uint32_t seg = 0; seg < c->n_chunks; seg++) {
            const uint32_t* rp = c->runs + ((size_t)p * c->n_chunks + seg) * 2;
            const char* rec = c->pay + (size_t)((int64_t)seg * c->chunk_rows + rp[0]) * c->rec;
            const int64_t run_ = rp[1];
            /* the runs lie in different chunks: fetch a few runs ahead */
            if (seg + AGG_RADIX_RUN_AHEAD < c->n_chunks) {
                const uint32_t* np = rp + 2 * AGG_RADIX_RUN_AHEAD;
                const char* nr = c->pay + (size_t)((int64_t)(seg + AGG_RADIX_RUN_AHEAD) * c->chunk_rows + np[0]) * c->rec;
                __builtin_prefetch(nr);
                __builtin_prefetch(nr + 64);
            }
            for (int64_t i = 0; i < run_; i++, rec += c->rec) {
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
        if (c->cmp_cap > 0) agg_radix_part_compact(c, &states, &first_row, &gkeys, &ng);
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
        c.have_thr = agg_topn_union_threshold(cand, nc, ng, ef, &c.thr);
    }
    if (pool) ray_pool_dispatch_n(pool, agg_radix_mark_fn, &c, n_parts);
    else agg_radix_mark_fn(&c, 0, 0, n_parts);
    int64_t kept = 0;
    for (uint32_t p = 0; p < n_parts; p++) kept += c.kept[p];
    ray_free_raw(cand); ray_free_raw(cand_n);
    /* The kept set is every group at or beyond the threshold.  Groups
     * strictly beyond it number fewer than N by construction, but the groups
     * AT the threshold can be nearly all of them — a count-per-group top-10
     * over a near-unique key has a threshold of 1 and every group ties it —
     * and sorting that set is a comparison sort over the whole grouping.
     * The emitted prefix is the tied groups' first-seen order, so only the
     * N tied groups with the smallest first_row can ever be taken: keep
     * exactly those, through a bounded max-heap, and the sort below runs
     * over at most 2N entries.  Without a threshold every group is kept
     * (the take exceeds the group count) and the set is small anyway. */
    int64_t take = c.have_thr ? ef->top_count_take : 0;
    int64_t sel_cap = c.have_thr ? 2 * take : kept;
    if (sel_cap <= 0) sel_cap = 1;
    agg_radix_order_t* sel = ray_alloc_raw((size_t)sel_cap * sizeof(agg_radix_order_t));
    int64_t* fr = ray_alloc_raw((size_t)sel_cap * sizeof(int64_t));
    if (!sel || !fr) {
        ray_free_raw(sel); ray_free_raw(fr);
        ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep); *rc = 1; return NULL;
    }
    int64_t k = 0;
    if (!c.have_thr) {
        for (uint32_t p = 0; p < n_parts; p++)
            for (int64_t gg = 0; gg < parts[p].ng; gg++)
                if (keep[base[p] + gg]) {
                    sel[k].idx = ((int64_t)p << 32) | (uint32_t)gg;
                    fr[k] = parts[p].first_row[gg];
                    k++;
                }
    } else {
        /* strict groups fill sel[0..k); the tied heap lives in sel[take..) */
        agg_radix_order_t* hp = sel + take;
        int64_t* hk = fr + take;
        int64_t hn = 0;
        bool overflow = false;
        for (uint32_t p = 0; p < n_parts && !overflow; p++) {
            const double* pv = vals + base[p];
            const uint8_t* pk = keep + base[p];
            const int64_t* pfr = parts[p].first_row;
            for (int64_t gg = 0; gg < parts[p].ng; gg++) {
                if (!pk[gg]) continue;
                int64_t payload = ((int64_t)p << 32) | (uint32_t)gg;
                if (pv[gg] != c.thr) {
                    if (k >= take) { overflow = true; break; }
                    sel[k].idx = payload; fr[k] = pfr[gg]; k++;
                    continue;
                }
                int64_t first = pfr[gg];
                if (hn < take) {
                    int64_t i = hn++;
                    hk[i] = first; hp[i].idx = payload;
                    while (i > 0) {                       /* sift up */
                        int64_t par = (i - 1) / 2;
                        if (hk[par] >= hk[i]) break;
                        int64_t tk = hk[par]; hk[par] = hk[i]; hk[i] = tk;
                        int64_t tp = hp[par].idx; hp[par].idx = hp[i].idx; hp[i].idx = tp;
                        i = par;
                    }
                } else if (first < hk[0]) {
                    hk[0] = first; hp[0].idx = payload;
                    int64_t i = 0;
                    for (;;) {                            /* sift down */
                        int64_t l = 2 * i + 1, r = l + 1, m = i;
                        if (l < take && hk[l] > hk[m]) m = l;
                        if (r < take && hk[r] > hk[m]) m = r;
                        if (m == i) break;
                        int64_t tk = hk[m]; hk[m] = hk[i]; hk[i] = tk;
                        int64_t tp = hp[m].idx; hp[m].idx = hp[i].idx; hp[i].idx = tp;
                        i = m;
                    }
                }
            }
        }
        if (overflow) {
            /* more than N groups beyond the threshold: not a threshold this
             * selection understands — let the caller take the full path */
            ray_free_raw(sel); ray_free_raw(fr);
            ray_free_raw(base); ray_free_raw(vals); ray_free_raw(keep); *rc = 2; return NULL;
        }
        /* close the gap between the strict prefix and the tied heap */
        if (k < take) {
            memmove(sel + k, hp, (size_t)hn * sizeof(agg_radix_order_t));
            memmove(fr + k, hk, (size_t)hn * sizeof(int64_t));
        }
        k += hn;
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

/* Largest group count whose first-seen order is restored by sorting the
 * groups rather than by the input-sized order map (see agg_parts_emit). */
#define AGG_ORDER_SORT_MAX_GROUPS 16384

/* Phase 3 of every partition-producing strategy (radix, table stack): order
 * the per-partition groups by first logical input position (or select the
 * bounded / top-N subset), emit the key columns from the packed per-group
 * keys and finalize the aggregates.  Consumes `parts` (destroyed + freed on
 * every path) and `d` (freed on every path). */
static ray_t* agg_parts_emit(ray_op_ext_t* ext, agg_desc_t* d,
        agg_radix_part_t* parts, uint32_t n_parts, ray_pool_t* pool, uint32_t nw,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        int64_t input_count, int64_t group_limit, const ray_group_emit_filter_t* efp) {
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    const int64_t* agg_syms = d->agg_syms;
    /* Phase 3: restore stable first-seen order across partitions. */
    int64_t ng = 0;
    for (uint32_t p = 0; p < n_parts; p++) ng += parts[p].ng;

    /* Place each group directly at its first logical input position, then
     * compact those occupied positions in one linear pass.  This restores
     * first-seen order without a comparison sort or a routing threshold. */

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
            route_stats.topn_kept = kept;
        } else if (rc == 1) {
            agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
            ray_free_raw(parts);
            agg_desc_free(d);
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
            ray_free_raw(parts);
            agg_desc_free(d);
            return rc == 1 ? ray_error("oom", NULL)
                           : ray_error("group", "failed to order radix groups");
        }
        pairs = sel_pairs;
    } else if (ng <= AGG_ORDER_SORT_MAX_GROUPS && ng * 32 <= input_count) {
        /* Few groups over many rows (every low-cardinality group-by): sort
         * the (first_row, group) pairs — first rows are distinct across
         * groups, so the order is total and identical to the map below —
         * instead of filling, scattering into and compacting an order map
         * of one word per INPUT row (32 MB and three passes for ten groups
         * over 4M rows). */
        pairs = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(agg_radix_order_t));
        ray_t* sk_hdr = NULL;
        int64_t* skeys = pairs ? (int64_t*)scratch_alloc(&sk_hdr,
            (size_t)(ng > 0 ? ng : 1) * sizeof(int64_t)) : NULL;
        if (!pairs || !skeys) {
            ray_free_raw(pairs); scratch_free(sk_hdr);
            agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
            ray_free_raw(parts);
            agg_desc_free(d);
            return ray_error("oom", NULL);
        }
        int64_t m = 0;
        for (uint32_t p = 0; p < n_parts; p++)
            for (int64_t gg = 0; gg < parts[p].ng; gg++, m++) {
                skeys[m] = parts[p].first_row[gg];
                pairs[m].idx = ((int64_t)p << 32) | (uint32_t)gg;
            }
        agg_sort_pairs_by_key(pairs, skeys, ng);
        scratch_free(sk_hdr);
    } else {
    pairs = ray_alloc_raw(
        (size_t)(input_count > 0 ? input_count : 1) * sizeof(agg_radix_order_t));
    if (!pairs) {
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        ray_free_raw(parts);
        agg_desc_free(d);
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
        ray_free_raw(parts);
        agg_desc_free(d);
        return ray_error("group", "failed to order radix groups");
    }
    }   /* end full-order path */
    ray_profile_tick("radix: ordered groups");

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        ray_free_raw(parts);
        agg_desc_free(d);
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
        ray_free_raw(parts);
        agg_desc_free(d);
        ray_release(result); return kerr;
    }
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_group_key_settle_nulls(kouts[k]);
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
        ray_free_raw(parts);
        agg_desc_free(d);
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
            ray_free_raw(parts);
            scratch_free(outs_hdr); agg_desc_free(d);
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
    ray_free_raw(parts);
    scratch_free(outs_hdr); agg_desc_free(d);
    return result;
}

/* Survivors per partition for a top-N emit (agg_radix_part_compact), or 0
 * when the emit keeps every group or selects with semantics the reduction
 * does not cover: a bounded first-seen emit, a minimum-count bound (it is
 * applied before the tie cut, so the kept groups are no longer a prefix of
 * the rank order), an aggregate without a scalar order, accumulators with a
 * destroy hook, or a take so large that the survivors are no small share of
 * the input. */
static int64_t agg_radix_cmp_cap(const ray_group_emit_filter_t* efp, int64_t group_limit,
                                 const agg_vtable_t** vts, uint32_t n_aggs,
                                 uint32_t n_parts, int64_t n_in) {
    if (ray_agg_radix_wave_bytes < 0 ||
        !efp || group_limit != 0 || efp->top_count_take <= 0 ||
        efp->min_count_exclusive > 0 || efp->agg_index >= n_aggs)
        return 0;
    switch (vts[efp->agg_index]->out_type) {
        case RAY_F64: case RAY_F32: case RAY_I64: case RAY_TIMESTAMP:
        case RAY_I32: case RAY_DATE: case RAY_TIME: case RAY_I16:
        case RAY_U8: case RAY_BOOL: break;
        default: return 0;
    }
    for (uint32_t a = 0; a < n_aggs; a++)
        if (vts[a]->destroy) return 0;
    if (efp->top_count_take > INT32_MAX) return 0;
    int64_t cap = 2 * efp->top_count_take;
    if ((double)cap * n_parts > (double)n_in / 2) return 0;
    return cap;
}

/* Bound on the waves a heap-derived plan may cut: every wave re-reads the
 * partition ids and the input rows, so past this many the passes cost more
 * than the memory they save. */
#define AGG_RADIX_MAX_WAVES 64

/* Cut the partitions into runs of consecutive partitions holding at most
 * `target` records each (a partition larger than that is a run of its own).
 * Returns the run count; cut[w] .. cut[w + 1] are run w's partitions and
 * *max_wave its largest record count. */
static uint32_t agg_radix_wave_cut(const int64_t* part_start, uint32_t n_parts,
                                   int64_t target, uint32_t* cut, int64_t* max_wave) {
    uint32_t k = 0, lo = 0;
    int64_t mx = 0;
    while (lo < n_parts) {
        uint32_t hi = lo + 1;
        while (hi < n_parts && part_start[hi + 1] - part_start[lo] <= target) hi++;
        cut[k++] = lo;
        if (part_start[hi] - part_start[lo] > mx) mx = part_start[hi] - part_start[lo];
        lo = hi;
    }
    cut[k] = n_parts;
    *max_wave = mx;
    return k;
}

/* Split the payload into waves of whole partitions when it would not fit the
 * memory left under the heap's anonymous watermark.  Only a reduced (top-N)
 * run benefits: its finished partitions shrink to their candidates, so the
 * largest wave's payload is what stays resident, not the whole input's.
 *
 * The room is what the watermark leaves above the committed memory, less
 * `reserve`: the candidates' bound, the tables of the partitions in flight
 * (one per worker, each a few bytes per row of the largest partition) and
 * the spill buffers when the later waves may go to a file.  The payload of
 * one wave gets the rest.  Waves are cut greedily on the exact partition
 * sizes; a heap-derived plan never cuts more than AGG_RADIX_MAX_WAVES (the
 * target doubles until it fits — a greedy cut can need up to twice the
 * waves the record count suggests).  The test knob fixes the target and
 * cuts as many waves as it asks for.  One wave = the one-pass scatter. */
static uint32_t agg_radix_wave_plan(const int64_t* part_start, uint32_t n_parts,
                                    size_t rec, int64_t cmp_cap, int64_t reserve,
                                    uint32_t* cut, int64_t* max_wave) {
    int64_t total = part_start[n_parts];
    cut[0] = 0; cut[1] = n_parts;
    *max_wave = total;
    if (cmp_cap <= 0 || total <= 0) return 1;
    if (ray_agg_radix_wave_bytes > 0) {
        int64_t target = ray_agg_radix_wave_bytes / (int64_t)rec;
        if (target < 1) target = 1;
        if (total <= target) return 1;
        return agg_radix_wave_cut(part_start, n_parts, target, cut, max_wave);
    }
    int64_t wm = ray_heap_anon_watermark();
    if (wm <= 0) return 1;   /* unknown RAM: one pass, as before */
    int64_t room = wm - ray_heap_anon_committed() - reserve;
    int64_t target = room > 0 ? room / (int64_t)rec : 0;
    if (total <= target) return 1;
    int64_t least = (total + AGG_RADIX_MAX_WAVES - 1) / AGG_RADIX_MAX_WAVES;
    if (target < least) target = least;
    for (;;) {
        uint32_t k = agg_radix_wave_cut(part_start, n_parts, target, cut, max_wave);
        if (k <= AGG_RADIX_MAX_WAVES) return k;
        target *= 2;
    }
}

/* Before the count pass: whether agg_radix_wave_plan may split a top-N run
 * of n_in records into waves — the test knob's bound is below the payload,
 * or the room under the heap's anonymous watermark is, less the plan's
 * reserve estimated with partitions of even size (the plan has the exact
 * sizes). */
static bool agg_radix_may_wave(int64_t n_in, uint32_t n_parts, size_t rec, size_t block,
                               uint32_t n_keys, uint32_t nw, int64_t cmp_cap) {
    if (cmp_cap <= 0 || n_in <= 0) return false;
    if (ray_agg_radix_wave_bytes > 0) {
        int64_t target = ray_agg_radix_wave_bytes / (int64_t)rec;
        return n_in > (target < 1 ? 1 : target);
    }
    int64_t wm = ray_heap_anon_watermark();
    if (wm <= 0) return false;
    size_t kb = 8 * (size_t)(n_keys ? n_keys : 1);
    double per_row = (double)(20 + 4 + rec + 2 * (block + kb + 24));
    double reserve = (double)cmp_cap * n_parts * (double)(block + kb + 8)
                   + (double)nw * ((double)n_in / n_parts) * per_row;
    double room = (double)(wm - ray_heap_anon_committed()) - reserve;
    return (double)n_in * (double)rec > room;
}

/* Whether a radix dispatch must not be followed by the next: the pool was
 * cancelled, or (a staged context) a range read failed. */
static bool agg_radix_stopped(const agg_radix_ctx_t* c, ray_pool_t* pool) {
    return pool_cancelled(pool) || (c->rr->staged && ray_scan_failed(c->rr->fail));
}

/* Count pass + prefix again: the partition ids and every chunk's write
 * cursors back to where the first scatter found them (part_start comes out
 * the same).  A spilled run whose file failed re-scans its remaining waves
 * from here.  A staged context counts through its key sources (count_rr);
 * a range error is noted in its failure envelope. */
static void agg_radix_recount(agg_radix_ctx_t* c, ray_pool_t* pool) {
    ray_pool_dispatch_n(pool, c->rr->staged ? agg_radix_count_staged_fn : agg_radix_count_fn,
                        c, c->n_chunks);
    uint32_t n_bands = (c->n_parts + AGG_RADIX_PREFIX_BAND - 1) / AGG_RADIX_PREFIX_BAND;
    if (n_bands > 1 && (uint64_t)c->n_chunks * c->n_parts >= ((uint64_t)1 << 16))
        ray_pool_dispatch_n(pool, agg_radix_prefix_fn, c, n_bands);
    else
        agg_radix_prefix_fn(c, 0, 0, n_bands);
    for (uint32_t p = 0; p < c->n_parts; p++) c->part_start[p + 1] += c->part_start[p];
}

/* Bytes of a column object that a re-scan would read from its file again:
 * a mapped plain column's payload, a generation-2 column's encoded file
 * (none for an in-memory stand-in), a parted column's segments; 0 for an
 * in-memory column.  Never decodes. */
static int64_t agg_col_file_bytes(const ray_t* col) {
    if (!col || RAY_IS_ERR(col)) return 0;
    if (col->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of((ray_t*)col);
        if (!e) return 0;
        if (e->kind == RAY_ENCCOL_SINGLE) return e->synthetic ? 0 : (int64_t)e->size;
        return agg_col_file_bytes(e->wrapper);
    }
    if (RAY_IS_PARTED(col->type)) {
        int64_t b = 0;
        ray_t* const* segs = (ray_t* const*)ray_data((ray_t*)col);
        for (int64_t s = 0; s < col->len; s++) b += agg_col_file_bytes(segs[s]);
        return b;
    }
    if (!ray_is_vec(col)) return 0;
    const ray_t* base = (col->attrs & RAY_ATTR_SLICE) && col->slice_parent ? col->slice_parent : col;
    return base->mmod == 1 ? col->len * (int64_t)col_esz(col) : 0;
}

/* Re-scan or spill the later waves?  A re-scan reads the input columns once
 * more per extra wave — from the page cache when they stay there, from their
 * files when they do not; a spill writes the later waves' records (with
 * their 4-byte slots) once and reads them back once.  With in-memory input
 * a re-scan moves no bytes to or from a device, so it always wins; with
 * file-backed input the cheaper of the two worst cases is taken, which caps
 * the re-scanned waves at about 2 * payload / input (four to five for a
 * near-unique two-key count). */
static bool agg_radix_spill_wanted(int64_t file_bytes, uint32_t n_waves,
                                   int64_t spilled_recs, size_t rec, int64_t max_wave) {
    if (!AGG_RADIX_FILE_SPILL || n_waves <= 1 || max_wave > (int64_t)UINT32_MAX ||
        ray_agg_radix_spill == 1)
        return false;
    if (ray_agg_radix_spill >= 2) return true;
    double spill = 2.0 * (double)spilled_recs * (double)(rec + sizeof(uint32_t));
    double rescan = (double)(n_waves - 1) * (double)file_bytes;
    return file_bytes > 0 && spill < rescan;
}

/* Run a split radix with its later waves spilled: one scatter pass writes
 * wave 0 into the payload and the rest to the file, wave 0 is reduced, then
 * each later wave is read back into the payload and reduced.  Returns the
 * first wave still to run by re-scan: n_waves when every wave was served,
 * 0 when the spill could not be set up (nothing scattered yet), or the wave
 * at which the file failed (the cursors are restored by agg_radix_recount).
 * `buf_bytes` is the share of the plan's reserve for the write and read-back
 * buffers: a buffer is never smaller than AGG_RADIX_SPILL_MIN_BUF (smaller
 * writes would be small random I/O), so when the workers' buffers of that
 * size for every wave exceed it, nothing is scattered and the caller
 * re-scans (returns 0). */
#define AGG_RADIX_SPILL_MIN_BUF ((int64_t)64 << 10)
static uint32_t agg_radix_spill_run(agg_radix_ctx_t* c, ray_pool_t* pool, uint32_t nw,
                                    const uint32_t* cut, uint32_t n_waves, int64_t buf_bytes,
                                    bool* cancelled, int* oom, uint64_t* bytes) {
    uint32_t n_parts = c->n_parts;
    size_t ent = c->rec + sizeof(uint32_t);
    int64_t per = buf_bytes / ((int64_t)nw * (n_waves + 1));
    if (per > ((int64_t)1 << 20)) per = (int64_t)1 << 20;
    if (per < AGG_RADIX_SPILL_MIN_BUF) per = AGG_RADIX_SPILL_MIN_BUF;
    uint32_t bcap = (uint32_t)(per / (int64_t)ent);
    if (bcap < 1) bcap = 1;
    size_t stride = ((size_t)bcap * ent + 7) & ~(size_t)7;   /* records stay 8-aligned */
    if ((double)nw * n_waves * (double)stride > (double)buf_bytes) return 0;
    route_stats.radix_spill_buf = (uint64_t)nw * n_waves * stride;
    int64_t spilled = c->part_start[n_parts] - c->part_start[cut[1]];
    agg_radix_spill_t sp = { .fd = -1, .n_waves = n_waves, .bcap = bcap, .rec = c->rec };
    atomic_init(&sp.n_blks, 0); atomic_init(&sp.tail, 0); atomic_init(&sp.err, 0);
    sp.blk_cap = spilled / bcap + (int64_t)nw * n_waves + 1;
    uint16_t* wave_of = ray_alloc_raw((size_t)n_parts * sizeof(uint16_t));
    int64_t* wave_base = ray_alloc_raw((size_t)n_waves * sizeof(int64_t));
    sp.bufs = ray_calloc_raw((size_t)nw * n_waves * sizeof(agg_radix_sbuf_t));
    sp.rstride = stride;
    char* bufmem = ray_alloc_raw((size_t)nw * (n_waves - 1) * stride);
    sp.rbuf = ray_alloc_raw((size_t)nw * stride);
    sp.blks = ray_alloc_raw((size_t)sp.blk_cap * sizeof(agg_radix_blk_t));
    int64_t* wave_blks = ray_alloc_raw((size_t)sp.blk_cap * sizeof(int64_t));
    sp.fd = (wave_of && wave_base && sp.bufs && bufmem && sp.rbuf && sp.blks && wave_blks)
          ? agg_radix_spill_open() : -1;
    if (sp.fd < 0) {
        ray_free_raw(wave_of); ray_free_raw(wave_base); ray_free_raw(sp.bufs);
        ray_free_raw(bufmem); ray_free_raw(sp.rbuf); ray_free_raw(sp.blks); ray_free_raw(wave_blks);
        return 0;
    }
    for (uint32_t w = 0; w < n_waves; w++) {
        wave_base[w] = c->part_start[cut[w]];
        for (uint32_t p = cut[w]; p < cut[w + 1]; p++) wave_of[p] = (uint16_t)w;
    }
    for (uint32_t t = 0; t < nw; t++)
        for (uint32_t w = 1; w < n_waves; w++)
            sp.bufs[(size_t)t * n_waves + w].buf =
                bufmem + ((size_t)t * (n_waves - 1) + (w - 1)) * stride;
    sp.wave_base = wave_base;
    sp.wave_blks = wave_blks;
    // cppcheck-suppress autoVariables // sp outlives every dispatch that reads it; c->spill is reset before return
    c->spill = &sp; c->wave_of = wave_of;

    uint32_t next = n_waves;
    ray_pool_dispatch_n(pool, c->rr->staged ? agg_radix_spill_scatter_staged_fn
                                           : agg_radix_spill_scatter_fn, c, c->n_chunks);
    *cancelled = agg_radix_stopped(c, pool);
    for (uint32_t t = 0; t < nw && !*cancelled; t++)
        for (uint32_t w = 1; w < n_waves; w++)
            agg_radix_spill_flush(&sp, &sp.bufs[(size_t)t * n_waves + w], w);
    ray_free_raw(bufmem); bufmem = NULL;
    ray_profile_tick("radix: spilled later waves");
    for (uint32_t w = 0; w < n_waves && !*cancelled && !*oom; w++) {
        uint32_t lo = cut[w], hi = cut[w + 1];
        if (w > 0) {
            if (atomic_load_explicit(&sp.err, memory_order_relaxed)) {
                agg_radix_recount(c, pool);   /* the file failed: re-scan from here */
                *cancelled = agg_radix_stopped(c, pool);
                next = w;
                break;
            }
            int64_t nb = 0, total = atomic_load_explicit(&sp.n_blks, memory_order_relaxed);
            for (int64_t i = 0; i < total; i++)
                if (sp.blks[i].wave == w) wave_blks[nb++] = i;
            if (nb > 0) ray_pool_dispatch_n(pool, agg_radix_readback_fn, c, (uint32_t)nb);
            *cancelled = agg_radix_stopped(c, pool);
            if (*cancelled) break;
            if (atomic_load_explicit(&sp.err, memory_order_relaxed)) {
                agg_radix_recount(c, pool);
                *cancelled = agg_radix_stopped(c, pool);
                next = w;
                break;
            }
        }
        if (c->part_start[hi] == c->part_start[lo]) continue;
        c->part_lo = lo;
        c->pay_base = c->part_start[lo];
        ray_pool_dispatch_n(pool, agg_radix_group_fn, c, hi - lo);
        *cancelled = agg_radix_stopped(c, pool);
        for (uint32_t p = lo; p < hi && !*cancelled; p++)
            if (c->parts[p].oom) { *oom = 1; break; }
        ray_profile_tick("radix: wave reduced");
    }
    *bytes = (uint64_t)atomic_load_explicit(&sp.tail, memory_order_relaxed);
#if AGG_RADIX_FILE_SPILL
    close(sp.fd);
#endif
    c->spill = NULL; c->wave_of = NULL; c->part_lo = 0; c->pay_base = 0;
    ray_free_raw(wave_of); ray_free_raw(wave_base); ray_free_raw(sp.bufs);
    ray_free_raw(sp.rbuf); ray_free_raw(sp.blks); ray_free_raw(wave_blks);
    return next;
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
    ray_t* derr = NULL;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols, &derr)) return derr ? derr : ray_error("oom", NULL);
    const void** key_data = d.key_data;
    /* Phase 1 reads the inputs through views; the layout and phase 2 test
     * their presence on the identities (staged or plain). */
    const void** val_data = d.val_id; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_id; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;

    /* Every group records its earliest source row so emitted order does not
     * depend on the number of radix partitions used by this execution. */
    bool needs_row = true;

    /* Per-row payload record layout: [packed keys][agg values][row_idx?].
     * val_off/val2_off are per-dispatch (filled once here, read by workers);
     * only phases 1-2 read them, freed once phase 2 joins. */
    ray_t* off_hdr;
    size_t* val_off = (size_t*)scratch_calloc(&off_hdr,
        2u * (size_t)(n_aggs ? n_aggs : 1) * sizeof(size_t) +
        ((size_t)n_keys + 2u * n_aggs) * sizeof(agg_radix_cf_t));
    if (!val_off) { agg_desc_free(&d); return ray_error("oom", NULL); }
    size_t* val2_off = val_off + n_aggs;
    agg_radix_cf_t* cf = (agg_radix_cf_t*)(val_off + 2u * (n_aggs ? n_aggs : 1));
    size_t rec_cur = (size_t)n_keys * 8;
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (val_data[a])  { val_off[a]  = rec_cur; rec_cur += val_esz[a]; }
        if (val2_data[a]) { val2_off[a] = rec_cur; rec_cur += val2_esz[a]; }
    }
    size_t row_off = rec_cur; if (needs_row) rec_cur += 8;
    size_t rec = (rec_cur + 7u) & ~(size_t)7u;   /* 8-align records */
    /* The compact staging of a fused pass (not sel mode: the scatter reads
     * the plain columns at the chunk's own rows): the record's fields in
     * the record's order (the keys, then each aggregate's x and y, as
     * val_off / val2_off lay them out), a staged source's in the staged
     * row, a plain one read in place.  Field j walks source i: the keys
     * are sources 0 .. n_keys, aggregate a's x n_keys + a, its y
     * n_keys + n_aggs + a. */
    bool compact = d.staged && !sel;
    size_t srec = 0;
    uint32_t n_cf = 0;
    for (uint32_t j = 0; compact && j < n_keys + 2u * n_aggs; j++) {
        bool key = j < n_keys;
        uint32_t a = key ? 0 : (j - n_keys) / 2;
        bool y = !key && ((j - n_keys) & 1);
        uint32_t i = key ? j : y ? n_keys + n_aggs + a : n_keys + a;
        if (!key && !(y ? val2_data[a] : val_data[a])) continue;     /* no input (COUNT) */
        agg_radix_cf_t* f = &cf[n_cf++];
        memset(f, 0, sizeof *f);
        f->agg = a;
        f->y = y;
        f->off = (uint32_t)(key ? (size_t)i * 8 : y ? val2_off[a] : val_off[a]);
        f->w = key ? 8 : y ? val2_esz[a] : val_esz[a];
        if (d.rr.srcs[i]) {
            f->kind = AGG_CF_STAGED;
            f->soff = (uint32_t)srec;
            srec += f->w;
            continue;
        }
        f->plain = d.rr.plain[i];
        f->esz = d.rr.esz[i];
        if (!f->plain) { compact = false; break; }
        if (!key) { f->kind = AGG_CF_VAL; continue; }
        switch (d.rr.key_types[i]) {
            case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: f->kind = AGG_CF_KEY_I64; break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: f->kind = AGG_CF_KEY_I32; break;
            case RAY_I16: f->kind = AGG_CF_KEY_I16; break;
            case RAY_U8: case RAY_BOOL: f->kind = AGG_CF_KEY_U8; break;
            case RAY_SYM:
                switch (d.rr.key_attrs[i] & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8:  f->kind = AGG_CF_KEY_U8; break;
                    case RAY_SYM_W16: f->kind = AGG_CF_KEY_U16; break;
                    case RAY_SYM_W32: f->kind = AGG_CF_KEY_U32; break;
                    default:          f->kind = AGG_CF_KEY_I64; break;
                }
                break;
            default: compact = false; break;                         /* the gate admits only the above */
        }
    }

    int64_t n_in = sel ? n_sel : nrows;
    if (n_in < 0) n_in = 0;
    uint32_t n_parts = agg_radix_part_count(nw, n_in);
    /* Partition ids are stored as uint16 between the passes.  Only inputs
     * past 2^32 rows ask for more partitions; they get larger ones. */
    if (n_parts > 65536) n_parts = 65536;

    /* Phase-1 chunks: a few per worker so claiming balances uneven rows,
     * bounded by the count-matrix budget and by a minimum chunk size, and
     * small enough that a chunk's per-partition count fits uint32. */
    uint64_t want = (uint64_t)nw * AGG_RADIX_CHUNKS_PER_WORKER;
    uint64_t by_budget = AGG_RADIX_COUNT_BUDGET / ((uint64_t)n_parts * sizeof(uint32_t));
    if (want > by_budget) want = by_budget;
    if (want < nw) want = nw;
    uint64_t by_rows = ((uint64_t)n_in + AGG_RADIX_CHUNK_MIN_ROWS - 1) / AGG_RADIX_CHUNK_MIN_ROWS;
    if (by_rows < nw && (uint64_t)n_in >= (uint64_t)nw * AGG_RADIX_CHUNK_FLOOR_ROWS)
        by_rows = nw;
    if (want > by_rows) want = by_rows;
    uint64_t by_width = (uint64_t)n_in / ((uint64_t)1 << 30) + 1;
    if (want < by_width) want = by_width;
    if (want < 1) want = 1;
    int64_t chunk_rows = (int64_t)(((uint64_t)n_in + want - 1) / want);
    if (chunk_rows < 1) chunk_rows = 1;
    /* Sources that decode (generation 2) are read once: the fused pass
     * stages each chunk in its task (agg_radix_fused_fn).  Its chunks are
     * small, so the staged records stay in the task's cache while they are
     * scattered: AGG_RADIX_RUN_ROWS records per partition on average (a
     * partition's runs, one per chunk, stay worth reading in phase 2), at
     * least one block.  The workers' staging buffers must fit the
     * last-level cache together (the table stack's bound), and the chunks
     * the count matrix; otherwise the two passes read the sources twice
     * (staging that spills costs more than the second decode). */
    bool fused = false;
    if (d.staged) {
        for (uint32_t i = 0; i < d.rr.n_src && !fused; i++)
            fused = d.rr.srcs[i] && ray_col_src_encoded(d.rr.srcs[i]);
        int64_t want_rows = (int64_t)n_parts * AGG_RADIX_RUN_ROWS;
        if (want_rows < RAY_COL_BLOCK_ROWS) want_rows = RAY_COL_BLOCK_ROWS;
        uint64_t llc_bytes = ray_cache_llc_bytes();
        double cache_budget = (llc_bytes ? (double)llc_bytes : 32.0 * 1048576.0) * 0.75;
        /* a worker's staging: a chunk's staged rows (compact: the staged
         * sources' fields, srec bytes; else whole records, rec) and
         * partition ids (2 bytes a row), and the partitions' write cursors */
        double per_worker = cache_budget / (double)nw - (double)n_parts * sizeof(uint32_t);
        double staged_row = (double)(compact ? srec : rec) + sizeof(uint16_t);
        int64_t cap = per_worker > 0 ? (int64_t)(per_worker / staged_row) : 0;
        /* Staging over its share keeps the fused pass on smaller chunks:
         * the largest block multiple under the cap, while that still holds
         * half the records a run wants (4 a partition on average; q17's
         * two keys: 3 of 4 blocks, decoded once).  Below half the short
         * runs cost more than the second decode (TS2m: 40-byte records
         * over 8192 partitions, a quarter). */
        if (fused) {
            int64_t target = chunk_rows > want_rows ? want_rows : chunk_rows;
            if (target > cap) {
                int64_t capped = cap / RAY_COL_BLOCK_ROWS * RAY_COL_BLOCK_ROWS;
                if (capped >= RAY_COL_BLOCK_ROWS && 2 * capped >= want_rows) target = capped;
                else fused = false;
            }
            if (fused) chunk_rows = target;
        }
        /* the count matrix and the runs (twice its size) live together
         * until the runs are written */
        uint64_t nch = ((uint64_t)n_in + (uint64_t)chunk_rows - 1) / (uint64_t)chunk_rows;
        if (fused && nch * n_parts * 3 * sizeof(uint32_t) > AGG_RADIX_COUNT_BUDGET) {
            fused = false;
            chunk_rows = (int64_t)(((uint64_t)n_in + want - 1) / want);   /* the two passes' chunks */
            if (chunk_rows < 1) chunk_rows = 1;
        }
    }
    /* A top-N emit keeps only its partitions' candidates (cmp_cap).  When
     * the whole payload may not fit the wave bound (agg_radix_may_wave),
     * the two passes run instead of the fused pass: their partition-major
     * payload splits into waves of whole partitions, while the fused pass's
     * chunk-major payload holds every record at once. */
    int64_t cmp_cap = needs_row
        ? agg_radix_cmp_cap(efp, group_limit, vts, n_aggs, n_parts, n_in) : 0;
    if (fused && agg_radix_may_wave(n_in, n_parts, rec, block, n_keys, nw, cmp_cap)) {
        fused = false;
        chunk_rows = (int64_t)(((uint64_t)n_in + want - 1) / want);   /* the two passes' chunks */
        if (chunk_rows < 1) chunk_rows = 1;
    }
    uint32_t n_chunks = (uint32_t)((n_in + chunk_rows - 1) / chunk_rows);

    /* O(1) allocations whatever the worker count: the count matrix and the
     * partition ids live until the last scatter pass ends (fused: no ids;
     * the count matrix until its runs are transposed, the runs until phase
     * 2 ends), the payload until phase 2 ends.  The fused payload holds the
     * whole input; the two passes size theirs once the partition sizes are
     * known: the whole input, or the largest wave. */
    agg_radix_part_t* parts = ray_calloc_raw((size_t)n_parts * sizeof(agg_radix_part_t));
    uint32_t* cnt = ray_alloc_raw((size_t)(n_chunks ? n_chunks : 1) * n_parts * sizeof(uint32_t));
    int64_t* part_start = ray_alloc_raw(((size_t)n_parts + 1) * sizeof(int64_t));
    uint16_t* pid = fused ? NULL : ray_alloc_raw((size_t)(n_in ? n_in : 1) * sizeof(uint16_t));
    uint32_t* wave_cut = fused ? NULL : ray_alloc_raw(((size_t)n_parts + 1) * sizeof(uint32_t));
    char* pay = fused ? ray_alloc_raw((size_t)(n_in ? n_in : 1) * rec) : NULL;
    if (!parts || !cnt || !part_start || (fused ? !pay : (!pid || !wave_cut))) {
        ray_free_raw(parts); ray_free_raw(cnt); ray_free_raw(part_start);
        ray_free_raw(pid); ray_free_raw(wave_cut); ray_free_raw(pay);
        scratch_free(off_hdr); agg_desc_free(&d);
        return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
    }
    part_start[0] = 0;

    agg_radix_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .nw = nw, .n_parts = n_parts,
        .part_bits = (uint32_t)__builtin_ctz(n_parts),
        .n_in = n_in, .chunk_rows = chunk_rows, .n_chunks = n_chunks,
        .cnt = cnt, .part_start = part_start, .pay = pay, .pid = pid,
        .parts = parts,
        .rec = rec, .row_off = row_off, .needs_row = needs_row,
        .sel = sel, .sel_prefix = sel_prefix,
        .val_off = val_off, .val2_off = val2_off,
        .wave_lo = 0, .wave_hi = n_parts, .part_lo = 0, .pay_base = 0,
        .cmp_cap = cmp_cap,
        .cmp_vt = cmp_cap ? vts[efp->agg_index] : NULL,
        .cmp_off = cmp_cap ? off[efp->agg_index] : 0,
        .cmp_param = cmp_cap && ext->agg_k ? ext->agg_k[efp->agg_index] : 0,
        .cmp_desc = cmp_cap ? efp->desc : 0,
        .rr = &d.rr, .whole = &d.whole,
        .compact = fused && compact, .srec = srec, .n_cf = n_cf, .cf = cf,
    };
    route_stats.radix_phase1 = !fused ? AGG_RADIX_P1_TWO_PASS
                             : ctx.compact ? AGG_RADIX_P1_FUSED_COMPACT : AGG_RADIX_P1_FUSED_RECORDS;
    atomic_init(&ctx.part_overflow, 0);
    atomic_init(&ctx.cmp_parts, 0);
    agg_desc_advise(&d, nrows);
    /* Pass 1 of the two passes reads the keys alone: an input is read once
     * by each scatter. */
    agg_rr_t count_rr = d.rr;
    ray_col_src_t** count_srcs = NULL;
    ctx.count_rr = &d.rr;
    if (d.staged && !fused) {
        count_srcs = ray_alloc_raw((size_t)(d.rr.n_src ? d.rr.n_src : 1) * sizeof(*count_srcs));
        if (count_srcs) {
            for (uint32_t i = 0; i < d.rr.n_src; i++) count_srcs[i] = i < n_keys ? d.rr.srcs[i] : NULL;
            count_rr.srcs = count_srcs;
            ctx.count_rr = &count_rr;
        }
    }

    if (fused) {
        /* One pass: each chunk counted and scattered inside its own record
         * range, then the partition totals. */
        uint32_t n_bands = (n_parts + AGG_RADIX_PREFIX_BAND - 1) / AGG_RADIX_PREFIX_BAND;
        route_stats.radix_waves = 1;
        ctx.runs = ray_alloc_raw((size_t)n_chunks * n_parts * 2 * sizeof(uint32_t));
        if (!ctx.runs) {
            ray_free_raw(parts); ray_free_raw(cnt); ray_free_raw(part_start); ray_free_raw(pay);
            scratch_free(off_hdr); agg_desc_free(&d);
            return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                    key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
        }
        ctx.fused_tasks = nw * AGG_RADIX_CHUNKS_PER_WORKER;
        if (ctx.fused_tasks > n_chunks) ctx.fused_tasks = n_chunks;
        if (ctx.fused_tasks < 1) ctx.fused_tasks = 1;
        ray_pool_dispatch_n(pool, agg_radix_fused_fn, &ctx, ctx.fused_tasks);
        ray_t* ferr = ray_scan_fail_finish(&d.fail, pool);
        bool cancelled = pool_cancelled(pool) || ferr;
        if (!cancelled) {
            if (n_bands > 1 && (uint64_t)n_chunks * n_parts >= ((uint64_t)1 << 16))
                ray_pool_dispatch_n(pool, agg_radix_fused_totals_fn, &ctx, n_bands);
            else
                agg_radix_fused_totals_fn(&ctx, 0, 0, n_bands);
            cancelled = pool_cancelled(pool);
        }
        ray_free_raw(cnt); ctx.cnt = cnt = NULL;     /* phase 2 reads the runs */
        int oom = atomic_load_explicit(&ctx.part_overflow, memory_order_relaxed);
        if (!cancelled && !oom)
            for (uint32_t p = 0; p < n_parts; p++) part_start[p + 1] += part_start[p];
        ray_profile_tick("radix: scattered rows");
        if (!cancelled && !oom)
            ray_pool_dispatch_n(pool, agg_radix_group_fused_fn, &ctx, n_parts);
        if (!cancelled && !oom) {
            cancelled = pool_cancelled(pool);
            for (uint32_t p = 0; p < n_parts; p++)
                if (parts[p].oom) { oom = 1; break; }
        }
        route_stats.radix_compacted =
            (uint64_t)atomic_load_explicit(&ctx.cmp_parts, memory_order_relaxed);
        ray_profile_tick("radix: reduced partitions");
        scratch_free(off_hdr);
        ray_free_raw(ctx.runs); ctx.runs = NULL;
        ray_free_raw(pay); ctx.pay = NULL;
        ray_free_raw(part_start); ctx.part_start = NULL;
        if (cancelled || oom) {
            agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
            ray_free_raw(parts);
            agg_desc_free(&d);
            if (ferr) return ferr;
            if (cancelled) return ray_error("cancel", NULL);
            return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                    key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
        }
        return agg_parts_emit(ext, &d, parts, n_parts, pool, nw, key_cols, key_syms,
                              vts, off, block, sel ? n_sel : nrows, group_limit, efp);
    }

    /* Phase 1, pass 1: per-(chunk, partition) counts.  Sel mode chunks the
     * selected-row space [0,n_sel); each chunk decodes its selected rows to
     * ORIGINAL indices.  A cancelled dispatch skips chunks and leaves their
     * count rows and ids unset, so nothing past it may run. */
    ray_pool_dispatch_n(pool, ctx.rr->staged ? agg_radix_count_staged_fn : agg_radix_count_fn, &ctx, n_chunks);
    ray_t* ferr = d.staged ? ray_scan_fail_finish(&d.fail, pool) : NULL;
    bool cancelled = pool_cancelled(pool) || ferr;
    if (!cancelled) {
        uint32_t n_bands = (n_parts + AGG_RADIX_PREFIX_BAND - 1) / AGG_RADIX_PREFIX_BAND;
        if (n_bands > 1 && (uint64_t)n_chunks * n_parts >= ((uint64_t)1 << 16))
            ray_pool_dispatch_n(pool, agg_radix_prefix_fn, &ctx, n_bands);
        else
            agg_radix_prefix_fn(&ctx, 0, 0, n_bands);
        cancelled = pool_cancelled(pool);
    }
    int oom = atomic_load_explicit(&ctx.part_overflow, memory_order_relaxed);
    uint32_t n_waves = 1;
    bool no_pay = false;
    int64_t file_bytes = 0, buf_bytes = 0;
    bool spill_chosen = false;
    if (!cancelled && !oom) {
        for (uint32_t p = 0; p < n_parts; p++) part_start[p + 1] += part_start[p];
        int64_t max_wave = n_in;
        /* Held beside one wave's payload: the candidates, the tables of the
         * partitions in flight (one per worker; per row of the largest
         * partition its hash slots, gid and value columns, and per group
         * — at most a row each, doubling capacity — its state, keys, key
         * pointer, first row and the reduction's value) and, with
         * file-backed input, the spill buffers. */
        int64_t max_part = 0;
        for (uint32_t p = 0; p < n_parts; p++)
            if (part_start[p + 1] - part_start[p] > max_part)
                max_part = part_start[p + 1] - part_start[p];
        size_t kb = 8 * (size_t)(n_keys ? n_keys : 1);
        int64_t per_row = (int64_t)(20 + 4 + rec + 2 * (block + kb + 24));
        int64_t reserve = cmp_cap * (int64_t)n_parts * (int64_t)(block + kb + 8)
                        + (int64_t)nw * max_part * per_row;
        /* The bytes a re-scan reads again: each key's and input's object as
         * the scan reads it (a staged source's bound object, else the plain
         * column read in place; an input read from an in-memory
         * materialization of an encoded column reads no file).  Never
         * decodes. */
        for (uint32_t k = 0; k < n_keys; k++)
            file_bytes += agg_col_file_bytes(d.key_srcs[k] ? d.key_srcs[k]->obj : key_cols[k]);
        for (uint32_t a = 0; a < n_aggs; a++) {
            ray_t* vc = NULL;
            if (d.val_srcs[a]) {
                vc = d.val_srcs[a]->obj;
            } else if (d.val_data[a] && d.agg_syms[a]) {
                ray_t* slot = ray_table_slot(tbl, d.agg_syms[a]);
                vc = slot && slot->type != RAY_ENCCOL ? slot : NULL;
            }
            bool dup = false;
            for (uint32_t k = 0; k < n_keys && vc; k++)
                if ((d.key_srcs[k] ? d.key_srcs[k]->obj : key_cols[k]) == vc) dup = true;
            if (!dup) file_bytes += agg_col_file_bytes(vc);
        }
        n_waves = agg_radix_wave_plan(part_start, n_parts, rec, cmp_cap, reserve,
                                      wave_cut, &max_wave);
        /* A split that would spill (by the cost rule, or the test knob) is
         * planned again with room for the write buffers (at most 1 MB per
         * worker and wave, at most an eighth of the room, none once the
         * watermark is passed).  If the larger reserve changes the plan so
         * that the spill no longer pays, the plan without buffers stands: a
         * re-scan needs none, and its waves are not multiplied for nothing. */
        if (n_waves > 1 && (file_bytes > 0 || ray_agg_radix_spill >= 2) &&
            agg_radix_spill_wanted(file_bytes, n_waves,
                                   part_start[n_parts] - part_start[wave_cut[1]], rec, max_wave)) {
            int64_t wm = ray_heap_anon_watermark();
            int64_t room = wm > 0 ? wm - ray_heap_anon_committed() : -1;
            int64_t bb = (int64_t)nw * (AGG_RADIX_MAX_WAVES + 1) << 20;
            if (wm > 0 && bb > (room > 0 ? room / 8 : 0)) bb = room > 0 ? room / 8 : 0;
            uint32_t w2 = bb > 0
                ? agg_radix_wave_plan(part_start, n_parts, rec, cmp_cap, reserve + bb,
                                      wave_cut, &max_wave)
                : 0;
            if (w2 > 1 && agg_radix_spill_wanted(file_bytes, w2,
                                   part_start[n_parts] - part_start[wave_cut[1]], rec, max_wave)) {
                n_waves = w2; buf_bytes = bb; spill_chosen = true;
            } else if (w2) {
                n_waves = agg_radix_wave_plan(part_start, n_parts, rec, cmp_cap, reserve,
                                              wave_cut, &max_wave);
            }
        }
        pay = ray_alloc_raw((size_t)(max_wave > 0 ? max_wave : 1) * rec);
        ctx.pay = pay;
        if (!pay) no_pay = true;
    }
    route_stats.radix_waves = n_waves;
    if (!cancelled && !oom && !no_pay && n_waves == 1) {
        /* Pass 2: every record to its exact slot. */
        ray_pool_dispatch_n(pool, ctx.rr->staged ? agg_radix_scatter_staged_fn : agg_radix_scatter_fn,
                            &ctx, n_chunks);
        cancelled = agg_radix_stopped(&ctx, pool);
        ray_free_raw(cnt); cnt = ctx.cnt = NULL;
        ray_free_raw(pid); pid = ctx.pid = NULL;
        ray_free_raw(count_srcs); count_srcs = NULL; ctx.count_rr = NULL;
        ray_profile_tick("radix: scattered rows");

        /* Phase 2: per-partition group+accumulate. */
        if (!cancelled)
            ray_pool_dispatch_n(pool, agg_radix_group_fn, &ctx, n_parts);
        if (!cancelled) {
            cancelled = pool_cancelled(pool);
            for (uint32_t p = 0; p < n_parts; p++)
                if (parts[p].oom) { oom = 1; break; }
        }
    } else if (!cancelled && !oom && !no_pay) {
        /* Waves: scatter the records of a run of partitions, reduce those
         * partitions to their top-N candidates, reuse the payload for the
         * next run.  The later waves come from the spill file when that
         * moves fewer bytes (agg_radix_spill_wanted); otherwise, or from the
         * wave at which the file failed, every wave re-reads the input (its
         * rows in place or, staged, by range; the partition ids
         * sequentially); no wave writes outside its payload. */
        uint32_t next = 0;
        if (spill_chosen) {
            uint64_t sb = 0;
            next = agg_radix_spill_run(&ctx, pool, nw, wave_cut, n_waves, buf_bytes,
                                       &cancelled, &oom, &sb);
            route_stats.radix_spill_bytes = sb;
        }
        for (uint32_t w = next; w < n_waves && !cancelled && !oom; w++) {
            uint32_t lo = wave_cut[w], hi = wave_cut[w + 1];
            if (part_start[hi] == part_start[lo]) continue;   /* only empty partitions */
            ctx.wave_lo = lo; ctx.wave_hi = hi; ctx.part_lo = lo;
            ctx.pay_base = part_start[lo];
            ray_pool_dispatch_n(pool, ctx.rr->staged ? agg_radix_wave_scatter_staged_fn
                                                     : agg_radix_wave_scatter_fn, &ctx, n_chunks);
            cancelled = agg_radix_stopped(&ctx, pool);
            if (cancelled) break;
            ray_pool_dispatch_n(pool, agg_radix_group_fn, &ctx, hi - lo);
            cancelled = pool_cancelled(pool);
            for (uint32_t p = lo; p < hi && !cancelled; p++)
                if (parts[p].oom) { oom = 1; break; }
            ray_profile_tick("radix: wave reduced");
        }
        ctx.wave_lo = 0; ctx.wave_hi = n_parts; ctx.part_lo = 0; ctx.pay_base = 0;
    }
    /* A range read that failed in a scatter is the query's error. */
    if (!ferr && d.staged && ray_scan_failed(&d.fail)) ferr = ray_scan_fail_finish(&d.fail, pool);
    if (ferr) cancelled = true;
    ray_free_raw(cnt); ctx.cnt = NULL;
    ray_free_raw(pid); ctx.pid = NULL;
    ray_free_raw(count_srcs); ctx.count_rr = NULL;
    ray_free_raw(wave_cut);
    route_stats.radix_compacted =
        (uint64_t)atomic_load_explicit(&ctx.cmp_parts, memory_order_relaxed);
    if (no_pay) oom = 1;

    ray_profile_tick("radix: reduced partitions");

    /* Phases 1+2 done (every dispatch joined); val_off/val2_off and the
     * payload records are no longer read — groups keep copies of their keys. */
    scratch_free(off_hdr);
    ray_free_raw(pay); ctx.pay = NULL;
    ray_free_raw(part_start); ctx.part_start = NULL;

    if (cancelled || oom) {
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        ray_free_raw(parts);
        agg_desc_free(&d);
        if (ferr) return ferr;
        if (cancelled) return ray_error("cancel", NULL);
        return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
    }

    return agg_parts_emit(ext, &d, parts, n_parts, pool, nw, key_cols, key_syms,
                          vts, off, block, sel ? n_sel : nrows, group_limit, efp);
}

/* ══════════════════════════════════════════════════════════════════════
 * Table-stack group-by (int/SYM keys, ACC_STREAMING aggs only).
 *
 * The radix strategy above scatters EVERY input row into a payload record
 * before any aggregation happens, so a 4M-row group-by over ten sparse keys
 * pays the full scatter (~100 MB of records) to produce ten groups.  This
 * strategy pre-aggregates first and scatters only when pre-aggregation
 * provably stops paying:
 *
 *   Gate:    a sample of the grouped rows (the SELECTED rows under a
 *            pushed filter) gives a Chao1 distinct estimate and the share
 *            of rows whose key the sample saw once.  The strategy takes
 *            the query only when the estimated groups fit one table of at
 *            most AGG_TS_MAX_BITS slots, that singleton share is bounded
 *            (Chao1 under-counts a long tail behind a hot head), the
 *            tables of all workers fit the last-level cache together, AND
 *            every phase-1 task is expected to fold several rows per
 *            group; otherwise it
 *            declines (returns NULL) and the caller runs the radix scatter,
 *            whose L1-local per-partition probes beat cache-missing table
 *            probes once rows per group per task drop below a handful
 *            (measured: 50k+ groups over 4M rows on 24 threads ran 0.55-
 *            0.85x of radix on the table stack, <= 15k groups 1.2-6x).
 *   Phase 1 (one logical task per contiguous input range; tasks are claimed
 *            by whichever worker is free, but the task -> range map is fixed
 *            so the merge order, and with it every floating-point result, is
 *            independent of scheduling): each task owns a stack of
 *            open-addressing tables.  Slot = hash >> (64 - log2 cap)
 *            (TOP-BIT placement), so for any power-of-two table the entries
 *            of merge partition p (the top log2(n_parts) hash bits) live in
 *            one contiguous slot range — the merge below range-scans the
 *            task tables directly, no scatter copy of their entries.  The
 *            hash lives inline in the entry; a one-byte tag array answers
 *            "empty?" and most "not this key" from L1.
 *            A table grows by doubling up to AGG_TS_MAX_BITS; a full table
 *            at that size is PUSHED onto the task's stack and a fresh one
 *            started (no rehash past cache size, no scatter).  Every time
 *            a table fills, the task compares the distinct entries it took
 *            against the rows it absorbed: at >= AGG_TS_SWITCH_PCT percent
 *            distinct the input is nearly all-distinct in this range (the
 *            sample estimate was wrong), so the task switches to RAW
 *            SCATTER — the remaining rows go as packed records into
 *            per-partition buckets (chunked bump allocation, lazily
 *            created) and are aggregated by the merge.
 *   Phase 2 (one task per merge partition): fold, in task order, every
 *            table's slot range for p and every task's bucket p into one
 *            partition-local hash; the result is the same agg_radix_part_t
 *            the radix strategy produces, so the shared agg_parts_emit
 *            handles ordering (first-seen), bounded emit, native top-N and
 *            the parallel key/aggregate emit unchanged.
 * ══════════════════════════════════════════════════════════════════════ */

#define AGG_TS_CHUNK          4096      /* rows per probe + accumulate batch */
#define AGG_TS_MIN_BITS       13        /* smallest table: 8192 slots */
#define AGG_TS_MAX_BITS       15        /* largest table: 32768 slots, then stack */
#define AGG_TS_LOAD_NUM       3         /* fill tables to 3/4 */
#define AGG_TS_SWITCH_ROWS    16384     /* rows a table must absorb before its
                                         * distinct ratio decides anything */
#define AGG_TS_SWITCH_PCT     50        /* distinct/rows (%) at which a filled
                                         * table switches its task to raw scatter */
#define AGG_TS_TASK_ROWS      16384     /* minimum rows per phase-1 task */
#define AGG_TS_TASKS_PER_WORKER 2
#define AGG_TS_BUCKET_FIRST   ((size_t)8 << 10)    /* first bucket chunk (bytes) */
#define AGG_TS_BUCKET_NEXT    ((size_t)64 << 10)   /* largest bucket chunk (bytes) */
#define AGG_TS_SAMPLE         8192      /* single rows sampled for the gate + sizing */
#define AGG_TS_GATE_UNSEEN_PCT 66       /* reject when more than this share of the
                                         * sampled rows carry a key seen once */
#define AGG_TS_GATE_GROUPS    24576     /* estimated groups must fit one full
                                         * AGG_TS_MAX_BITS table */
#define AGG_TS_GATE_REPEAT    4         /* ...and each task must expect this
                                         * many rows per group */

/* Entry layout at ent + slot * estride:
 *   [0, 8)                 uint64 key hash (| 1, never 0 when occupied)
 *   [8, 16)                int64 first logical input position
 *   [16, 16 + 8*n_keys)    packed keys (agg_read_key_i64 widened)
 *   [state_off, ...)       AoS aggregate states (off[a] inside) */
#define AGG_TS_KEY_OFF        16
typedef struct agg_ts_table {
    uint8_t*             tag;     /* [cap] 0 = empty, else hash bits 48..55 | 1 */
    char*                ent;     /* [cap * estride] */
    uint32_t             bits;
    int64_t              cap;
    int64_t              ng;      /* occupied slots */
    int64_t              rows;    /* rows aggregated into this table */
    struct agg_ts_table* prev;    /* older, full table of the same task */
} agg_ts_table_t;

/* Scatter bucket chunk: records follow the header (24 bytes → 8-aligned). */
typedef struct agg_ts_chunk {
    struct agg_ts_chunk* next;
    int64_t              n;       /* records used */
    int64_t              cap;     /* records that fit */
} agg_ts_chunk_t;

typedef struct {
    agg_ts_chunk_t* head;
    agg_ts_chunk_t* tail;
    int64_t         n;            /* records in the bucket */
    size_t          bytes;        /* size of the last chunk (doubles per chunk) */
} agg_ts_bucket_t;

typedef struct {
    agg_ts_table_t*  top;         /* current table; prev links the stack */
    agg_ts_bucket_t* buckets;     /* [n_parts], allocated on the switch */
    int64_t          rows;        /* rows consumed by this task */
    int64_t          entries;     /* entries across the task's tables */
    int64_t          scattered;   /* rows that went to buckets */
    uint32_t         n_tables;
    bool             scatter;     /* raw scatter mode */
    int              oom;
} agg_ts_task_t;

typedef struct {
    ray_t**              key_cols;
    const void**         key_data;
    uint32_t             n_keys;
    const agg_vtable_t** vts;
    const size_t*        off;
    size_t               block;
    uint32_t             n_aggs;
    const void**         val_data;  const int8_t* val_types;  const bool* val_hasnull;  const uint8_t* val_esz;
    const void**         val2_data; const int8_t* val2_types; const bool* val2_hasnull; const uint8_t* val2_esz;
    /* Table-stride view of the aggregates: states live inside the entries. */
    agg_valdesc_t        vd;
    const size_t*        ent_off;   /* [n_aggs] state_off + off[a] */
    size_t               estride;
    size_t               state_off;
    /* Scatter record layout (same as the radix payload): [keys][vals][pos]. */
    size_t               rec;
    size_t               row_off;
    const size_t*        val_off;
    const size_t*        val2_off;
    uint32_t             n_parts;
    uint32_t             part_bits;
    uint32_t             init_bits;
    int64_t              n_in;
    int64_t              task_rows;
    uint32_t             n_tasks;
    agg_ts_task_t*       tasks;     /* [n_tasks] */
    agg_radix_part_t*    parts;     /* [n_parts] */
    int64_t              merge_cand; /* total entries + scattered rows (phase 2 sizing) */
    ray_t*               sel;
    const int64_t*       sel_prefix;
    const agg_rr_t*      rr;        /* sources; staged ones are read by range */
    const agg_view_t*    whole;
} agg_ts_ctx_t;

/* Same FNV-1a + fmix64 as the radix scatter; bit 0 forced on so 0 can mark
 * an empty slot (top-bit placement and partition bits are unaffected). */
static inline uint64_t agg_ts_hash(const int64_t* keys, uint32_t n_keys) {
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < n_keys; k++) { h ^= (uint64_t)keys[k]; h *= 1099511628211ULL; }
    return agg_radix_fmix64(h) | 1u;
}

/* Tag byte: hash bits 48..55 (below the slot/partition bits), never 0. */
static inline uint8_t agg_ts_tag(uint64_t h) { return (uint8_t)((h >> 48) | 1u); }

static inline uint64_t agg_ts_ent_hash(const agg_ts_table_t* t, size_t estride, int64_t slot) {
    uint64_t h; memcpy(&h, t->ent + (size_t)slot * estride, sizeof(h)); return h;
}

static agg_ts_table_t* agg_ts_table_new(uint32_t bits, size_t estride) {
    agg_ts_table_t* t = ray_alloc_raw(sizeof(agg_ts_table_t));
    if (!t) return NULL;
    int64_t cap = (int64_t)1 << bits;
    t->tag = ray_calloc_raw((size_t)cap);
    t->ent = ray_alloc_raw((size_t)cap * estride);
    if (!t->tag || !t->ent) {
        ray_free_raw(t->tag); ray_free_raw(t->ent); ray_free_raw(t);
        return NULL;
    }
    t->bits = bits; t->cap = cap; t->ng = 0; t->rows = 0; t->prev = NULL;
    return t;
}

static void agg_ts_table_free(agg_ts_table_t* t) {
    while (t) {
        agg_ts_table_t* prev = t->prev;
        ray_free_raw(t->tag); ray_free_raw(t->ent); ray_free_raw(t);
        t = prev;
    }
}

/* Double a table in place of itself (same stack position).  Top-bit
 * placement keeps the relative order of the entries; the states are plain
 * bytes (streaming accumulators own no memory) so a memcpy moves them. */
static agg_ts_table_t* agg_ts_table_grow(agg_ts_table_t* t, size_t estride) {
    agg_ts_table_t* n = agg_ts_table_new(t->bits + 1, estride);
    if (!n) return NULL;
    uint64_t nmask = (uint64_t)n->cap - 1;
    uint32_t shift = 64 - n->bits;
    for (int64_t s = 0; s < t->cap; s++) {
        if (!t->tag[s]) continue;
        uint64_t h = agg_ts_ent_hash(t, estride, s);
        uint64_t d = h >> shift;
        while (n->tag[d]) d = (d + 1) & nmask;
        n->tag[d] = t->tag[s];
        memcpy(n->ent + (size_t)d * estride, t->ent + (size_t)s * estride, estride);
    }
    n->ng = t->ng; n->rows = t->rows; n->prev = t->prev;
    ray_free_raw(t->tag); ray_free_raw(t->ent); ray_free_raw(t);
    return n;
}

static void agg_ts_bucket_free(agg_ts_bucket_t* b) {
    agg_ts_chunk_t* ch = b->head;
    while (ch) { agg_ts_chunk_t* nx = ch->next; ray_free_raw(ch); ch = nx; }
    b->head = b->tail = NULL; b->n = 0; b->bytes = 0;
}

static void agg_ts_task_free(agg_ts_task_t* ts, uint32_t n_parts) {
    agg_ts_table_free(ts->top); ts->top = NULL;
    if (ts->buckets) {
        for (uint32_t p = 0; p < n_parts; p++) agg_ts_bucket_free(&ts->buckets[p]);
        ray_free_raw(ts->buckets); ts->buckets = NULL;
    }
}

/* Reserve one record slot in bucket b (lazy, chunked bump allocation). */
static inline char* agg_ts_bucket_push(agg_ts_bucket_t* b, size_t rec) {
    agg_ts_chunk_t* ch = b->tail;
    if (!ch || ch->n == ch->cap) {
        /* Chunks double from AGG_TS_BUCKET_FIRST to AGG_TS_BUCKET_NEXT, so a
         * sparse stream (tasks x partitions of them) wastes at most about
         * what it holds while a dense one soon bumps 64 KB at a time. */
        size_t bytes = ch ? b->bytes * 2 : AGG_TS_BUCKET_FIRST;
        if (bytes > AGG_TS_BUCKET_NEXT) bytes = AGG_TS_BUCKET_NEXT;
        b->bytes = bytes;
        int64_t cap = (int64_t)((bytes - sizeof(agg_ts_chunk_t)) / rec);
        if (cap < 1) cap = 1;
        agg_ts_chunk_t* nc = ray_alloc_raw(sizeof(agg_ts_chunk_t) + (size_t)cap * rec);
        if (!nc) return NULL;
        nc->next = NULL; nc->n = 0; nc->cap = cap;
        if (ch) ch->next = nc; else b->head = nc;
        b->tail = nc; ch = nc;
    }
    b->n++;
    return (char*)(ch + 1) + (size_t)ch->n++ * rec;
}

/* Make room in the task's current table for up to `n` new entries at the
 * configured load: grow below the size cap, push at the cap, or switch the
 * task to raw scatter when the filled table shows the input is too distinct
 * for pre-aggregation to pay.  Returns false on OOM. */
static bool agg_ts_reserve(agg_ts_ctx_t* c, agg_ts_task_t* ts, int64_t n) {
    agg_ts_table_t* t = ts->top;
    while (!t || t->ng + n > (t->cap >> 2) * AGG_TS_LOAD_NUM) {
        if (t && t->rows >= AGG_TS_SWITCH_ROWS &&
                t->ng * 100 >= t->rows * AGG_TS_SWITCH_PCT) {
            ts->scatter = true;
            return true;
        }
        if (!t) {
            t = agg_ts_table_new(c->init_bits, c->estride);
            if (!t) return false;
            ts->n_tables++;
        } else if (t->bits < AGG_TS_MAX_BITS) {
            agg_ts_table_t* grown = agg_ts_table_grow(t, c->estride);
            if (!grown) return false;   /* t is intact; the task free releases it */
            t = grown;
        } else {
            agg_ts_table_t* nt = agg_ts_table_new(AGG_TS_MAX_BITS, c->estride);
            if (!nt) return false;
            nt->prev = t; t = nt;
            ts->n_tables++;
        }
        ts->top = t;
    }
    return true;
}

/* One chunk of a task, probed and accumulated through a view: indices
 * [ib, ie) of the chunk, original rows rows[i] (or pos0 + i when rows ==
 * NULL) at logical input positions pos0 + i.  The chunk's table room was
 * reserved (or the task switched to scatter) for the whole chunk first, so
 * its pieces see the same table and the same switch point as one call. */
typedef struct {
    agg_ts_ctx_t*      c;
    agg_ts_task_t*     ts;
    const int64_t*     rows;
    int64_t            pos0;
    agg_sel_scratch_t* sc;
    uint32_t*          gid;
    int64_t*           keys;
} agg_ts_run_t;

static void agg_ts_idx_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t ib, int64_t ie) {
    (void)wid;
    agg_ts_run_t* run = raw;
    agg_ts_ctx_t* c = run->c;
    agg_ts_task_t* ts = run->ts;
    const int64_t* rows = run->rows;
    const int64_t pos0 = run->pos0, base = v->base;
    int64_t* keys = run->keys;
    uint32_t* gid = run->gid;
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    if (ts->oom) return;
    /* The entry and record stores below are byte copies, which may alias
     * any field: the keys' read parameters are hoisted into locals (a
     * single key's into registers). */
    uint32_t nkv = n_keys ? n_keys : 1;
    int8_t kt[nkv]; uint8_t ka[nkv]; const void* kd[nkv];
    for (uint32_t k = 0; k < n_keys; k++) {
        kt[k] = c->key_cols[k]->type; ka[k] = v->key_attrs[k]; kd[k] = v->key_data[k];
    }
    const int8_t kt0 = kt[0];
    const uint8_t ka0 = ka[0];
    const void* kd0 = kd[0];
    #define AGG_TS_KEYS(r) do {                                                       \
        if (n_keys == 1) keys[0] = agg_read_key_i64_raw(kt0, ka0, kd0, (r) - base);   \
        else for (uint32_t k = 0; k < n_keys; k++)                                    \
            keys[k] = agg_read_key_i64_raw(kt[k], ka[k], kd[k], (r) - base);         \
    } while (0)

    if (ts->scatter) {
        uint32_t pshift = 64 - c->part_bits;
        for (int64_t i = ib; i < ie; i++) {
            int64_t r = rows ? rows[i] : pos0 + i;
            AGG_TS_KEYS(r);
            uint64_t h = agg_ts_hash(keys, n_keys);
            char* rec = agg_ts_bucket_push(&ts->buckets[h >> pshift], c->rec);
            if (!rec) { ts->oom = 1; return; }
            memcpy(rec, keys, (size_t)n_keys * sizeof(int64_t));
            for (uint32_t a = 0; a < n_aggs; a++) {
                if (v->val_data[a]) {
                    uint8_t ez = c->val_esz[a];
                    memcpy(rec + c->val_off[a], (const char*)v->val_data[a] + (size_t)(r - base) * ez, ez);
                }
                if (v->val2_data[a]) {
                    uint8_t ez2 = c->val2_esz[a];
                    memcpy(rec + c->val2_off[a], (const char*)v->val2_data[a] + (size_t)(r - base) * ez2, ez2);
                }
            }
            int64_t pos = pos0 + i;
            memcpy(rec + c->row_off, &pos, sizeof(pos));
        }
        return;
    }

    agg_ts_table_t* t = ts->top;
    uint64_t mask = (uint64_t)t->cap - 1;
    uint32_t shift = 64 - t->bits;
    size_t estride = c->estride, key_bytes = (size_t)n_keys * sizeof(int64_t);
    for (int64_t i = ib; i < ie; i++) {
        int64_t r = rows ? rows[i] : pos0 + i;
        AGG_TS_KEYS(r);
        uint64_t h = agg_ts_hash(keys, n_keys);
        uint8_t tag = agg_ts_tag(h);
        uint64_t slot = h >> shift;
        for (;;) {
            uint8_t st = t->tag[slot];
            if (!st) {
                char* e = t->ent + (size_t)slot * estride;
                t->tag[slot] = tag;
                int64_t pos = pos0 + i;      /* positions ascend within a task:
                                              * the first sight IS the minimum */
                memcpy(e, &h, sizeof(h));
                memcpy(e + sizeof(uint64_t), &pos, sizeof(pos));
                memcpy(e + AGG_TS_KEY_OFF, keys, key_bytes);
                for (uint32_t a = 0; a < n_aggs; a++)
                    c->vts[a]->init(e + c->ent_off[a]);
                t->ng++;
                break;
            }
            if (st == tag) {
                const char* e = t->ent + (size_t)slot * estride;
                uint64_t eh; memcpy(&eh, e, sizeof(eh));
                if (eh == h && memcmp(e + AGG_TS_KEY_OFF, keys, key_bytes) == 0) break;
            }
            slot = (slot + 1) & mask;
        }
        gid[i - ib] = (uint32_t)slot;
    }
    int64_t n = ie - ib;
    if (rows) {
        if (agg_sel_accum_chunk(&c->vd, v, run->sc, t->ent, gid, rows + ib, n, false) != 0) { ts->oom = 1; return; }
    } else {
        int64_t at = pos0 + ib - base;
        for (uint32_t a = 0; a < n_aggs; a++) {
            char* states = t->ent + c->ent_off[a];
            if (c->vts[a]->update_batch2) {
                const void* vx = (const char*)v->val_data[a]  + (size_t)at * c->val_esz[a];
                const void* vy = (const char*)v->val2_data[a] + (size_t)at * c->val2_esz[a];
                ray_valid_t valid_x = { vx, c->val_types[a],  c->val_hasnull[a] };
                ray_valid_t valid_y = { vy, c->val2_types[a], c->val2_hasnull[a] };
                c->vts[a]->update_batch2(states, estride, gid, vx, vy, &valid_x, &valid_y, n, NULL);
            } else if (v->val_data[a]) {
                const void* vals = (const char*)v->val_data[a] + (size_t)at * c->val_esz[a];
                ray_valid_t valid = { vals, c->val_types[a], c->val_hasnull[a] };
                c->vts[a]->update_batch(states, estride, gid, vals, &valid, n, NULL);
            } else {
                ray_valid_t valid = { NULL, c->val_types[a], false };
                c->vts[a]->update_batch(states, estride, gid, NULL, &valid, n, NULL);
            }
        }
    }
    #undef AGG_TS_KEYS
}

/* A piece of rows [b, e) of a contiguous chunk, as chunk indices. */
static void agg_ts_range_rows(void* raw, const agg_view_t* v, uint32_t wid, int64_t b, int64_t e) {
    agg_ts_run_t* run = raw;
    agg_ts_idx_rows(raw, v, wid, b - run->pos0, e - run->pos0);
}

/* Visit one chunk of rows of a task: original rows `rows[i]` (or pos0 + i
 * when rows == NULL) at logical input positions pos0 + i.  Staged sources
 * are read by pieces of the chunk (through the task's cursors `cs` when
 * given), or by runs of its selected rows through them.  False on a range
 * error (noted keyed by `key`). */
static bool agg_ts_chunk(agg_ts_ctx_t* c, agg_ts_task_t* ts,
                         const int64_t* rows, int64_t pos0, int64_t n,
                         agg_sel_scratch_t* sc, uint32_t* gid, int64_t* keys,
                         uint32_t wid, agg_curs_t* cs, int64_t key) {
    const agg_rr_t* rr = c->rr;
    if (!ts->scatter && !agg_ts_reserve(c, ts, n)) { ts->oom = 1; return true; }
    if (ts->scatter && !ts->buckets) {
        ts->buckets = ray_calloc_raw((size_t)c->n_parts * sizeof(agg_ts_bucket_t));
        if (!ts->buckets) { ts->oom = 1; return true; }
    }
    agg_ts_run_t run = { c, ts, rows, pos0, sc, gid, keys };
    if (!rr->staged) agg_ts_idx_rows(&run, c->whole, wid, 0, n);
    else if (rows) {
        ray_t* err = agg_idx_walk(rr, cs, rows, 0, n, &run, agg_ts_idx_rows, wid);
        if (err) { ray_scan_fail_note(rr->fail, key, err); return false; }
    } else if (cs ? !agg_walk_cs(rr, cs, &run, agg_ts_range_rows, wid, pos0, pos0 + n, key)
                  : !agg_walk(rr, &run, agg_ts_range_rows, wid, pos0, pos0 + n, key)) return false;
    if (ts->oom) return true;
    if (ts->scatter) { ts->rows += n; ts->scattered += n; }
    else { ts->top->rows += n; ts->rows += n; }
    return true;
}

/* Phase 1: one task = one contiguous range of logical input positions. */
static void agg_ts_phase1_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_ts_ctx_t* c = (agg_ts_ctx_t*)vctx;
    const agg_rr_t* rr = c->rr;
    ray_t* kb_hdr = NULL;
    int64_t* keys = (int64_t*)scratch_alloc(&kb_hdr,
        (size_t)(c->n_keys ? c->n_keys : 1) * sizeof(int64_t));
    uint32_t* gid = keys ? ray_alloc_raw(AGG_TS_CHUNK * sizeof(uint32_t)) : NULL;
    if (!keys || !gid) {
        for (int64_t ti = start; ti < end; ti++) c->tasks[ti].oom = 1;
        ray_free_raw(gid); scratch_free(kb_hdr);
        return;
    }
    for (int64_t ti = start; ti < end; ti++) {
        agg_ts_task_t* ts = &c->tasks[ti];
        int64_t lo = ti * c->task_rows;
        int64_t hi = lo + c->task_rows;
        if (hi > c->n_in) hi = c->n_in;
        if (rr->staged && ray_scan_fail_skip(rr->fail, lo)) continue;
        if (c->sel) {
            int64_t rows[AGG_SEL_CHUNK];
            agg_sel_scratch_t sc;
            if (!agg_sel_scratch_init(&sc, c->n_aggs)) { ts->oom = 1; continue; }
            agg_curs_t cs; cs.cur = NULL;
            if (rr->staged && !agg_curs_init(&cs, rr)) { ts->oom = 1; agg_sel_scratch_free(&sc); continue; }
            agg_sel_cursor_t cur;
            agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, lo, hi);
            int64_t pos = lo, cn;
            while (!ts->oom && (cn = agg_sel_cursor_next(&cur, rows)) > 0) {
                if (!agg_ts_chunk(c, ts, rows, pos, cn, &sc, gid, keys, wid, &cs, lo)) break;
                pos += cn;
            }
            agg_curs_free(&cs, rr);
            agg_sel_scratch_free(&sc);
        } else {
            agg_curs_t tc; tc.cur = NULL;
            if (rr->staged && !agg_curs_init(&tc, rr)) { ts->oom = 1; continue; }
            for (int64_t cs = lo; cs < hi && !ts->oom; cs += AGG_TS_CHUNK) {
                int64_t ce = cs + AGG_TS_CHUNK; if (ce > hi) ce = hi;
                if (!agg_ts_chunk(c, ts, NULL, cs, ce - cs, NULL, gid, keys, wid, rr->staged ? &tc : NULL, lo)) break;
            }
            agg_curs_free(&tc, rr);
        }
        for (agg_ts_table_t* tab = ts->top; tab; tab = tab->prev) ts->entries += tab->ng;
    }
    ray_free_raw(gid); scratch_free(kb_hdr);
}

/* Partition-local merge table (phase 2): hash-indexed gid, grow-on-demand
 * group arrays in the agg_radix_part_t layout. */
typedef struct {
    uint64_t* mh;        /* [htcap] key hash | 1, 0 = empty */
    int32_t*  mg;        /* [htcap] slot -> gid */
    int64_t   htcap;
    char*     states;    /* [gcap * block] */
    int64_t*  first_row; /* [gcap] */
    int64_t*  keys;      /* [gcap * n_keys] */
    int64_t   gcap, ng;
} agg_ts_merge_t;

static bool agg_ts_merge_init(agg_ts_merge_t* m, int64_t expect, size_t block, uint32_t n_keys) {
    int64_t htcap = 64;
    while (htcap < expect * 4) htcap <<= 1;
    int64_t gcap = expect < 256 ? expect : 256;
    if (gcap < 16) gcap = 16;
    m->mh = ray_calloc_raw((size_t)htcap * sizeof(uint64_t));
    m->mg = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
    m->states = ray_alloc_raw((size_t)gcap * block);
    m->first_row = ray_alloc_raw((size_t)gcap * sizeof(int64_t));
    m->keys = ray_alloc_raw((size_t)gcap * (size_t)(n_keys ? n_keys : 1) * sizeof(int64_t));
    m->htcap = htcap; m->gcap = gcap; m->ng = 0;
    return m->mh && m->mg && m->states && m->first_row && m->keys;
}

static void agg_ts_merge_free(agg_ts_merge_t* m, bool keep_groups) {
    ray_free_raw(m->mh); ray_free_raw(m->mg); m->mh = NULL; m->mg = NULL;
    if (!keep_groups) {
        ray_free_raw(m->states); ray_free_raw(m->first_row); ray_free_raw(m->keys);
        m->states = NULL; m->first_row = NULL; m->keys = NULL;
    }
}

/* Slot index inside the merge table: bits above the forced bit 0 (the top
 * bits are the partition, constant inside one table). */
#define AGG_TS_MSLOT(h, mask) (((h) >> 1) & (mask))

static bool agg_ts_merge_rehash(agg_ts_merge_t* m) {
    int64_t ncap = m->htcap * 2;
    uint64_t* nh = ray_calloc_raw((size_t)ncap * sizeof(uint64_t));
    int32_t* ng = ray_alloc_raw((size_t)ncap * sizeof(int32_t));
    if (!nh || !ng) { ray_free_raw(nh); ray_free_raw(ng); return false; }
    uint64_t nmask = (uint64_t)ncap - 1;
    for (int64_t s = 0; s < m->htcap; s++) {
        uint64_t h = m->mh[s];
        if (!h) continue;
        uint64_t d = AGG_TS_MSLOT(h, nmask);
        while (nh[d]) d = (d + 1) & nmask;
        nh[d] = h; ng[d] = m->mg[s];
    }
    ray_free_raw(m->mh); ray_free_raw(m->mg);
    m->mh = nh; m->mg = ng; m->htcap = ncap;
    return true;
}

/* Find or create the group of (h, keys) in the merge table; `pos` is the
 * candidate's first logical position (kept as the MIN).  Returns the gid,
 * -1 on OOM. */
static inline int64_t agg_ts_merge_gid(agg_ts_merge_t* m, agg_ts_ctx_t* c,
                                       uint64_t h, const int64_t* keys, int64_t pos) {
    uint32_t n_keys = c->n_keys;
    size_t key_bytes = (size_t)n_keys * sizeof(int64_t);
    uint64_t mask = (uint64_t)m->htcap - 1;
    uint64_t slot = AGG_TS_MSLOT(h, mask);
    for (;;) {
        uint64_t sh = m->mh[slot];
        if (!sh) break;
        int32_t gg = m->mg[slot];
        if (sh == h && memcmp(m->keys + (size_t)gg * n_keys, keys, key_bytes) == 0) {
            if (pos < m->first_row[gg]) m->first_row[gg] = pos;
            return gg;
        }
        slot = (slot + 1) & mask;
    }
    if ((m->ng + 1) * 2 > m->htcap) {          /* keep the load under 1/2 */
        if (!agg_ts_merge_rehash(m)) return -1;
        mask = (uint64_t)m->htcap - 1;
        slot = AGG_TS_MSLOT(h, mask);
        while (m->mh[slot]) slot = (slot + 1) & mask;
    }
    if (m->ng == m->gcap) {
        int64_t ncap = m->gcap * 2;
        void* ns = ray_realloc_raw(m->states, (size_t)ncap * c->block);
        if (!ns) return -1;
        m->states = ns;
        void* nf = ray_realloc_raw(m->first_row, (size_t)ncap * sizeof(int64_t));
        if (!nf) return -1;
        m->first_row = nf;
        void* nk = ray_realloc_raw(m->keys, (size_t)ncap * (size_t)(n_keys ? n_keys : 1) * sizeof(int64_t));
        if (!nk) return -1;
        m->keys = nk;
        m->gcap = ncap;
    }
    int64_t gg = m->ng++;
    m->mh[slot] = h; m->mg[slot] = (int32_t)gg;
    m->first_row[gg] = pos;
    memcpy(m->keys + (size_t)gg * n_keys, keys, key_bytes);
    for (uint32_t a = 0; a < c->n_aggs; a++)
        c->vts[a]->init(m->states + (size_t)gg * c->block + c->off[a]);
    return gg;
}

/* Slot range of partition p in a table: [lo, lo + n) plus the run of
 * occupied slots that linear probing pushed past it (wrapping).  Visits
 * every occupied slot of the range and of that run; the body filters by
 * partition (entries displaced INTO the range from the previous partition
 * and the run's own tail belong elsewhere). */
#define AGG_TS_RANGE_FOREACH(tab, p, part_bits, SLOT, BODY) do {               \
        agg_ts_table_t* tab_ = (tab);                                          \
        uint64_t mask_ = (uint64_t)tab_->cap - 1;                              \
        uint32_t span_ = tab_->bits - (part_bits);                             \
        int64_t lo_ = (int64_t)(p) << span_, n_ = (int64_t)1 << span_;         \
        int64_t s_ = lo_, end_ = lo_ + n_;                                     \
        for (; s_ < end_; s_++) {                                              \
            if (!tab_->tag[s_]) continue;                                      \
            int64_t SLOT = s_; BODY                                            \
        }                                                                      \
        for (int64_t k_ = 0; k_ < tab_->cap && tab_->tag[(uint64_t)s_ & mask_]; \
             k_++, s_++) { int64_t SLOT = (int64_t)((uint64_t)s_ & mask_); BODY } \
    } while (0)

/* Phase 2: merge partition p from every task, in task order (tables oldest
 * first, then the task's scattered records in input order).  Frees each
 * bucket it consumed; tables are shared by all partitions and outlive the
 * dispatch. */
static void agg_ts_merge_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_ts_ctx_t* c = (agg_ts_ctx_t*)vctx;
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    uint32_t pshift = 64 - c->part_bits;
    ray_t* ptr_hdr = NULL;
    /* Per-call scratch: gid batch, one gather buffer per aggregate side, and
     * a small array to walk a stack oldest-first. */
    char** gv = (char**)scratch_calloc(&ptr_hdr,
        (size_t)(2u * (n_aggs ? n_aggs : 1)) * sizeof(char*));
    uint32_t* gid = gv ? ray_alloc_raw(AGG_TS_CHUNK * sizeof(uint32_t)) : NULL;
    bool ok = gv && gid;
    char** gy = gv ? gv + (n_aggs ? n_aggs : 1) : NULL;
    for (uint32_t a = 0; ok && a < n_aggs; a++) {
        if (c->val_data[a])  { gv[a] = ray_alloc_raw((size_t)AGG_TS_CHUNK * (c->val_esz[a] ? c->val_esz[a] : 1));  ok = gv[a] != NULL; }
        if (ok && c->val2_data[a]) { gy[a] = ray_alloc_raw((size_t)AGG_TS_CHUNK * (c->val2_esz[a] ? c->val2_esz[a] : 1)); ok = gy[a] != NULL; }
    }
    uint32_t depth_cap = 16;
    agg_ts_table_t** stack = ok ? ray_alloc_raw(depth_cap * sizeof(agg_ts_table_t*)) : NULL;
    ok = ok && stack;
    int64_t expect = c->merge_cand / c->n_parts + 1;

    for (int64_t pi = start; pi < end; pi++) {
        uint32_t p = (uint32_t)pi;
        agg_radix_part_t* pr = &c->parts[p];
        if (!ok) { pr->oom = 1; continue; }

        agg_ts_merge_t m;
        if (!agg_ts_merge_init(&m, expect, c->block, n_keys)) {
            agg_ts_merge_free(&m, false); pr->oom = 1; continue;
        }
        bool poom = false;
        for (uint32_t ti = 0; ti < c->n_tasks && !poom; ti++) {
            agg_ts_task_t* ts = &c->tasks[ti];
            /* Tables oldest first. */
            uint32_t depth = 0;
            for (agg_ts_table_t* tab = ts->top; tab; tab = tab->prev) {
                if (depth == depth_cap) {
                    void* ns = ray_realloc_raw((void*)stack, (size_t)depth_cap * 2 * sizeof(agg_ts_table_t*));
                    if (!ns) { poom = true; break; }
                    stack = ns; depth_cap *= 2;
                }
                stack[depth++] = tab;
            }
            if (poom) break;
            for (uint32_t di = depth; di-- > 0 && !poom; ) {
                agg_ts_table_t* tab = stack[di];
                AGG_TS_RANGE_FOREACH(tab, p, c->part_bits, s, {
                    if (poom) break;
                    const char* e = tab_->ent + (size_t)s * c->estride;
                    uint64_t h; memcpy(&h, e, sizeof(h));
                    if ((uint32_t)(h >> pshift) == p) {
                        int64_t pos; memcpy(&pos, e + sizeof(uint64_t), sizeof(pos));
                        int64_t gg = agg_ts_merge_gid(&m, c, h,
                            (const int64_t*)(e + AGG_TS_KEY_OFF), pos);
                        if (gg < 0) { poom = true; break; }
                        for (uint32_t a = 0; a < n_aggs; a++)
                            c->vts[a]->merge(m.states + (size_t)gg * c->block + c->off[a],
                                             e + c->ent_off[a], NULL);
                    }
                });
            }
            if (poom || !ts->buckets) continue;
            /* Scattered records of this task, in input order, in batches. */
            agg_ts_bucket_t* b = &ts->buckets[p];
            for (agg_ts_chunk_t* ch = b->head; ch && !poom; ch = ch->next) {
                const char* rec = (const char*)(ch + 1);
                for (int64_t base = 0; base < ch->n && !poom; base += AGG_TS_CHUNK) {
                    int64_t bn = ch->n - base; if (bn > AGG_TS_CHUNK) bn = AGG_TS_CHUNK;
                    for (int64_t i = 0; i < bn; i++, rec += c->rec) {
                        const int64_t* keys = (const int64_t*)rec;
                        int64_t pos; memcpy(&pos, rec + c->row_off, sizeof(pos));
                        for (uint32_t a = 0; a < n_aggs; a++) {
                            if (gv[a]) memcpy(gv[a] + (size_t)i * c->val_esz[a],  rec + c->val_off[a],  c->val_esz[a]);
                            if (gy[a]) memcpy(gy[a] + (size_t)i * c->val2_esz[a], rec + c->val2_off[a], c->val2_esz[a]);
                        }
                        int64_t gg = agg_ts_merge_gid(&m, c, agg_ts_hash(keys, n_keys), keys, pos);
                        if (gg < 0) { poom = true; break; }
                        gid[i] = (uint32_t)gg;
                    }
                    if (poom) break;
                    for (uint32_t a = 0; a < n_aggs; a++) {
                        char* states = m.states + c->off[a];
                        if (c->vts[a]->update_batch2) {
                            ray_valid_t vx = { gv[a], c->val_types[a],  c->val_hasnull[a] };
                            ray_valid_t vy = { gy[a], c->val2_types[a], c->val2_hasnull[a] };
                            c->vts[a]->update_batch2(states, c->block, gid, gv[a], gy[a], &vx, &vy, bn, NULL);
                        } else if (c->val_data[a]) {
                            ray_valid_t valid = { gv[a], c->val_types[a], c->val_hasnull[a] };
                            c->vts[a]->update_batch(states, c->block, gid, gv[a], &valid, bn, NULL);
                        } else {
                            ray_valid_t valid = { NULL, c->val_types[a], false };
                            c->vts[a]->update_batch(states, c->block, gid, NULL, &valid, bn, NULL);
                        }
                    }
                }
            }
            agg_ts_bucket_free(b);   /* consumed by this partition only */
        }
        if (poom) { agg_ts_merge_free(&m, false); pr->oom = 1; continue; }
        agg_ts_merge_free(&m, true);
        pr->states = m.states; pr->first_row = m.first_row; pr->keys = m.keys; pr->ng = m.ng;
    }
    ray_free_raw(stack);
    for (uint32_t a = 0; a < n_aggs && gv; a++) { ray_free_raw(gv[a]); ray_free_raw(gy[a]); }
    ray_free_raw(gid); scratch_free(ptr_hdr);
}

/* Gate sample: AGG_TS_SAMPLE_TASKS slices of AGG_TS_SAMPLE_PER single rows,
 * each slice hashed into its own small open-addressing set by one pool
 * task (the reads are cache misses at scattered positions, and with a
 * selection each position costs a cursor placement; serial they were
 * 1-2 ms, more than a short radix query can afford).  The slices are
 * merged into one count set afterwards. */
#define AGG_TS_SAMPLE_TASKS   16
#define AGG_TS_SAMPLE_PER     (AGG_TS_SAMPLE / AGG_TS_SAMPLE_TASKS)
#define AGG_TS_SAMPLE_SETCAP  (AGG_TS_SAMPLE_PER * 4)
/* Serial pre-check on the calling thread before the parallel sample, in
 * two stages over scattered single rows.  Stage 1: AGG_TS_PRE_SAMPLE rows;
 * at most AGG_TS_PRE_REPEATS repeated keys among them declines outright
 * (the birthday estimate n^2 / (2 repeats) is then >= 128k groups, five
 * times the admission cap), so a near-unique key costs a few hundred
 * reads and no dispatch.  Stage 2 runs only when stage 1 is ambiguous
 * (AGG_TS_PRE_REPEATS < repeats < AGG_TS_PRE_CLEAR, a birthday estimate
 * between ~4k and ~65k groups): AGG_TS_PRE_SAMPLE2 rows in all, and a
 * birthday estimate of at least AGG_TS_PRE_DECLINE_X times the cap
 * declines — a 50k-group key repeats ~10 times among 1024 rows (declined
 * at <= 14, ~88% of the time), a key at the cap ~21 times (declined ~5%),
 * one at 15k ~35 times (never), so the 2x-5x-the-cap band rarely pays for
 * the parallel sample while low cardinalities pay only stage 1. */
#define AGG_TS_PRE_SAMPLE     512
#define AGG_TS_PRE_REPEATS    1
#define AGG_TS_PRE_CLEAR      32
#define AGG_TS_PRE_SAMPLE2    1024
#define AGG_TS_PRE_DECLINE_X  1.5
typedef struct {
    const agg_rr_t* rr;    /* the keys are its first n_keys sources */
    uint32_t       n_keys;
    int64_t        n_in;
    int64_t        step;
    ray_t*         sel;
    const int64_t* sel_prefix;
    uint64_t*      hs;     /* [tasks * SETCAP] key hash | 1, 0 = empty */
    uint16_t*      cnt;    /* [tasks * SETCAP] */
    int64_t*       keys;   /* [tasks * n_keys] packed-key scratch */
    int64_t*       n;      /* [tasks] rows sampled */
} agg_ts_sample_ctx_t;

static void agg_ts_sample_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_ts_sample_ctx_t* c = (agg_ts_sample_ctx_t*)vctx;
    const agg_rr_t* rr = c->rr;
    uint32_t n_keys = c->n_keys;
    for (int64_t t = start; t < end; t++) {
        uint64_t* hs = c->hs + (size_t)t * AGG_TS_SAMPLE_SETCAP;
        uint16_t* cnt = c->cnt + (size_t)t * AGG_TS_SAMPLE_SETCAP;
        int64_t* keys = c->keys + (size_t)t * (n_keys ? n_keys : 1);
        int64_t rows[AGG_SEL_CHUNK];
        int64_t n = 0;
        /* staged keys: strided single rows, ascending, through this task's
         * cursors (R-SPARSE) */
        agg_curs_t cs; cs.cur = NULL;
        ray_t* err = NULL;
        if (rr->staged && !agg_curs_init(&cs, rr)) err = ray_error("oom", NULL);
        for (int64_t j = 0; j < AGG_TS_SAMPLE_PER && !err; j++) {
            int64_t i = t * AGG_TS_SAMPLE_PER + j;
            int64_t pos = i * c->step + (int64_t)(ray_hash_i64(i) % (uint64_t)c->step);
            int64_t r = pos;
            if (c->sel) {
                agg_sel_cursor_t cur;
                agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, pos, pos + 1);
                if (agg_sel_cursor_next(&cur, rows) != 1) continue;
                r = rows[0];
            }
            for (uint32_t k = 0; k < n_keys && !err; k++)
                keys[k] = agg_key_at(rr, &cs, k, r, &err);
            if (err) break;
            uint64_t h = agg_ts_hash(keys, n_keys);
            uint64_t at = h & (AGG_TS_SAMPLE_SETCAP - 1);
            while (hs[at] && hs[at] != h) at = (at + 1) & (AGG_TS_SAMPLE_SETCAP - 1);
            if (!hs[at]) hs[at] = h;
            cnt[at]++;
            n++;
        }
        agg_curs_free(&cs, rr);
        if (err) ray_scan_fail_note(rr->fail, t, err);
        c->n[t] = n;
    }
}

/* agg_ts_sample_fn over staged keys: the same rows (the slice's selected
 * rows ascending), each key read a vector at a time: the slice's rows in
 * one vector point-read together (ray_col_src_points_i64: the body read
 * once, not once per row), a declined vector through the cursor.  Its own
 * instance: the plain sampler keeps its code. */
static void agg_ts_sample_staged_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_ts_sample_ctx_t* c = (agg_ts_sample_ctx_t*)vctx;
    const agg_rr_t* rr = c->rr;
    const uint32_t n_keys = c->n_keys;
    for (int64_t t = start; t < end; t++) {
        uint64_t* hs = c->hs + (size_t)t * AGG_TS_SAMPLE_SETCAP;
        uint16_t* cnt = c->cnt + (size_t)t * AGG_TS_SAMPLE_SETCAP;
        int64_t rows[AGG_TS_SAMPLE_PER], one[AGG_SEL_CHUNK];
        int64_t nr = 0;
        for (int64_t j = 0; j < AGG_TS_SAMPLE_PER; j++) {
            int64_t i = t * AGG_TS_SAMPLE_PER + j;
            int64_t pos = i * c->step + (int64_t)(ray_hash_i64(i) % (uint64_t)c->step);
            int64_t r = pos;
            if (c->sel) {
                agg_sel_cursor_t cur;
                agg_sel_cursor_init(&cur, c->sel, c->sel_prefix, pos, pos + 1);
                if (agg_sel_cursor_next(&cur, one) != 1) continue;
                r = one[0];
            }
            rows[nr++] = r;
        }
        agg_curs_t cs; cs.cur = NULL;
        ray_t* err = NULL;
        /* the slice's tuples, key-major: kv[k * nr + a] (the task's one scratch) */
        int64_t* kv = (int64_t*)ray_alloc_raw((size_t)(nr ? nr : 1) * (n_keys ? n_keys : 1) * sizeof(int64_t));
        if (!kv || !agg_curs_init(&cs, rr)) err = ray_error("oom", NULL);
        for (uint32_t k = 0; k < n_keys && !err; k++) {
            int64_t* vals = kv + (size_t)k * nr;
            ray_col_src_t* src = rr->srcs[k];
            for (int64_t a = 0; a < nr && !err; ) {
                int64_t vlo, vhi, b = a + 1;
                if (src && ray_col_src_vec_span(src, rows[a], &vlo, &vhi)) {
                    while (b < nr && b - a < RAY_COL_POINT_ROWS && rows[b] < vhi) b++;
                    int pr = agg_src_points(src, rows + a, b - a, vals + a, &err);
                    if (pr < 0) break;
                    if (pr > 0) { a = b; continue; }
                }
                vals[a] = agg_key_at(rr, &cs, k, rows[a], &err);   /* in place, or the cursor */
                a++;
            }
        }
        agg_curs_free(&cs, rr);
        int64_t n = 0;
        int64_t* keys = c->keys + (size_t)t * (n_keys ? n_keys : 1);
        for (int64_t a = 0; a < nr && !err; a++) {
            for (uint32_t k = 0; k < n_keys; k++) keys[k] = kv[(size_t)k * nr + a];
            uint64_t h = agg_ts_hash(keys, n_keys);
            uint64_t at = h & (AGG_TS_SAMPLE_SETCAP - 1);
            while (hs[at] && hs[at] != h) at = (at + 1) & (AGG_TS_SAMPLE_SETCAP - 1);
            if (!hs[at]) hs[at] = h;
            cnt[at]++;
            n++;
        }
        ray_free_raw(kv);
        if (err) ray_scan_fail_note(rr->fail, t, err);
        c->n[t] = n;
    }
}

/* Serial pre-check on the calling thread, before any allocation or
 * dispatch: AGG_TS_PRE_SAMPLE scattered single rows (selected rows under a
 * filter); true when at most AGG_TS_PRE_REPEATS keys repeat among them,
 * i.e. the key is near-unique and the strategy must decline. */
/* A selected row of the segment holding ORIGINAL position `pos`, read
 * straight from the selection: the row itself in an ALL segment, a
 * pseudo-random entry (by `salt`) of a MIX segment's index slice — one
 * cache line for the slice and one for the key, where a cursor placement
 * by selected ordinal costs a prefix search plus a decode.  False for a
 * NONE segment. */
static inline bool agg_ts_sel_row_at(const uint8_t* flags, const uint32_t* offsets,
                                     const uint16_t* idx, int64_t pos, uint64_t salt,
                                     int64_t* row) {
    uint32_t seg = (uint32_t)(pos / RAY_MORSEL_ELEMS);
    uint8_t f = flags[seg];
    if (f == RAY_SEL_NONE) return false;
    if (f == RAY_SEL_ALL) { *row = pos; return true; }
    uint32_t len = offsets[seg + 1] - offsets[seg];
    if (len == 0) return false;
    const uint16_t* slice = idx + offsets[seg];
    *row = (int64_t)seg * RAY_MORSEL_ELEMS + slice[salt % len];
    return true;
}

/* krr: the keys (staged ones read through cursors, scattered single rows);
 * a range error sets *err (the caller returns it). */
static bool agg_ts_precheck_declines(const agg_rr_t* krr, ray_t** key_cols, uint32_t n_keys, int64_t nrows,
                                     ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
                                     ray_t** err) {
    (void)sel_prefix;
    *err = NULL;
    enum { PRECAP = AGG_TS_PRE_SAMPLE2 * 4 };
    int64_t n_in = sel ? n_sel : nrows;
    if (n_in < AGG_TS_SAMPLE) return false;
    const uint8_t* sflags = sel ? ray_rowsel_flags(sel) : NULL;
    const uint32_t* soffsets = sel ? ray_rowsel_offsets(sel) : NULL;
    const uint16_t* sidx = sel ? ray_rowsel_idx(sel) : NULL;
    size_t nk = n_keys ? n_keys : 1;
    uint64_t phs[PRECAP];
    int64_t pkeys[16];
    ray_t* pk_hdr = NULL;
    int64_t* keys = pkeys;
    if (nk > 16) {
        keys = (int64_t*)scratch_alloc(&pk_hdr, nk * sizeof(int64_t));
        if (!keys) return false;
    }
    agg_curs_t cs; cs.cur = NULL;
    if (krr->staged && !agg_curs_init(&cs, krr)) { scratch_free(pk_hdr); return false; }
    memset(phs, 0, sizeof(phs));
    /* Stride AGG_TS_PRE_SAMPLE2 positions over the ORIGINAL rows (a selected
     * row is taken at or after each position); stage 1 visits every other
     * one (spread over the whole input), stage 2 fills in the rest.  A
     * position whose segment has no selected row is skipped, so a decision
     * needs at least half the intended rows; otherwise the full gate, which
     * samples by selected ordinal, decides. */
    const int64_t ratio = AGG_TS_PRE_SAMPLE2 / AGG_TS_PRE_SAMPLE;
    int64_t pstep = nrows / AGG_TS_PRE_SAMPLE2;
    if (pstep < 1) pstep = 1;
    if (!sel && !krr->staged)    /* staged keys are read by range instead */
        for (uint32_t k = 0; k < n_keys; k++)
            if (ray_col_cold(key_cols[k]))
                for (int64_t i = 0; i < AGG_TS_PRE_SAMPLE2; i++) {
                    int64_t pos = i * pstep + (int64_t)(ray_hash_i64(i + 7919) % (uint64_t)pstep);
                    if (pos < nrows) ray_col_want_row(key_cols[k], pos);
                }
    int64_t d = 0, n = 0;
    bool declined = false;
    for (int64_t phase = 0; phase < ratio && !declined; phase++) {
        for (int64_t j = 0; j < AGG_TS_PRE_SAMPLE; j++) {
            int64_t i = j * ratio + phase;
            uint64_t jit = ray_hash_i64(i + 7919);
            int64_t pos = i * pstep + (int64_t)(jit % (uint64_t)pstep);
            if (pos >= nrows) break;
            int64_t r = pos;
            if (sel && !agg_ts_sel_row_at(sflags, soffsets, sidx, pos, jit >> 32, &r)) continue;
            for (uint32_t k = 0; k < n_keys && !*err; k++)
                keys[k] = agg_key_at(krr, &cs, k, r, err);
            if (*err) { agg_curs_free(&cs, krr); scratch_free(pk_hdr); return false; }
            uint64_t h = agg_ts_hash(keys, n_keys);
            uint64_t at = h & (PRECAP - 1);
            while (phs[at] && phs[at] != h) at = (at + 1) & (PRECAP - 1);
            if (!phs[at]) { phs[at] = h; d++; }
            n++;
        }
        if (n < AGG_TS_PRE_SAMPLE / 2) break;   /* too few selected: full gate decides */
        int64_t repeats = n - d;
        if (phase == 0) {
            if (repeats <= AGG_TS_PRE_REPEATS) declined = true;
            else if (repeats >= AGG_TS_PRE_CLEAR) break;   /* clearly low: no stage 2 */
        }
    }
    if (!declined && n >= AGG_TS_PRE_SAMPLE2 / 2) {
        int64_t repeats = n - d;
        double birthday = repeats > 0 ? (double)n * (double)n / (2.0 * (double)repeats) : 9.2e18;
        declined = birthday >= AGG_TS_PRE_DECLINE_X * (double)AGG_TS_GATE_GROUPS;
    }
    agg_curs_free(&cs, krr);
    scratch_free(pk_hdr);
    return declined;
}

/* Chao1 estimate of the distinct key count from AGG_TS_SAMPLE SINGLE rows
 * at evenly spaced positions (hashed jitter inside each stride).  One row
 * per position, never a run of neighbours: real tables are often sorted
 * or sessionised, so a key with millions of distinct values still repeats
 * in short runs of consecutive rows, and a sample that reads neighbours
 * counts those runs as repeats — the estimate collapses and a group-by
 * with millions of groups is admitted (measured 1.3-1.9x slower than the
 * radix scatter).  Rows far apart only repeat when the key really is
 * frequent.  With a selection the sample is drawn from the SELECTED rows
 * (ordinals decoded through the selection cursor): the grouped keys are
 * those rows, and a sample of the whole table can look low-cardinality
 * while the surviving rows are near-unique (a `where: (!= phrase "")`
 * over a column that is mostly empty).  Chao1 is a lower bound for skewed
 * abundances, so *unseen_pct also reports the Good-Turing share of rows
 * whose key was seen once — the mass of keys the sample never saw — for
 * the caller's gate.  Returns a value past INT64_MAX/2 when the sample is
 * all-distinct. */
static double agg_ts_estimate(ray_pool_t* pool, const agg_rr_t* rr, ray_t** key_cols,
                              uint32_t n_keys, int64_t nrows,
                              ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
                              int* unseen_pct, ray_t** err) {
    *err = NULL;
    enum { CAP = 16384, PRECAP = AGG_TS_PRE_SAMPLE * 4 };
    int64_t n_in = sel ? n_sel : nrows;
    *unseen_pct = 0;
    if (n_in < AGG_TS_SAMPLE) return (double)n_in;
    size_t nk = n_keys ? n_keys : 1;
    size_t set_bytes = (size_t)AGG_TS_SAMPLE_TASKS * AGG_TS_SAMPLE_SETCAP;
    ray_t* hdr = NULL;
    uint64_t* hs = (uint64_t*)scratch_calloc(&hdr,
        set_bytes * sizeof(uint64_t) + set_bytes * sizeof(uint16_t)
        + (size_t)AGG_TS_SAMPLE_TASKS * nk * sizeof(int64_t)
        + (size_t)AGG_TS_SAMPLE_TASKS * sizeof(int64_t)
        + CAP * sizeof(uint64_t) + CAP * sizeof(uint16_t));
    if (!hs) return (double)n_in;
    uint16_t* cnt = (uint16_t*)(hs + set_bytes);
    int64_t* keys = (int64_t*)(cnt + set_bytes);
    int64_t* ns = keys + (size_t)AGG_TS_SAMPLE_TASKS * nk;
    uint64_t* ghs = (uint64_t*)(ns + AGG_TS_SAMPLE_TASKS);
    uint16_t* gcnt = (uint16_t*)(ghs + CAP);
    agg_ts_sample_ctx_t ctx = {
        .rr = rr, .n_keys = n_keys,
        .n_in = n_in, .step = n_in / AGG_TS_SAMPLE,
        .sel = sel, .sel_prefix = sel_prefix,
        .hs = hs, .cnt = cnt, .keys = keys, .n = ns,
    };
    /* A cold mapped key: ask for the sampled rows' pages first (the
     * staged sources of generation 2 are read by range instead). */
    if (!sel && !rr->staged)
        for (uint32_t k = 0; k < n_keys; k++)
            if (ray_col_cold(key_cols[k]))
                for (int64_t i = 0; i < AGG_TS_SAMPLE; i++)
                    ray_col_want_row(key_cols[k],
                        i * ctx.step + (int64_t)(ray_hash_i64(i) % (uint64_t)ctx.step));
    bool par = ray_pool_par_dispatch_ok(pool, AGG_TS_SAMPLE, 1);
    if (par)
        ray_pool_dispatch_n(pool, rr->staged ? agg_ts_sample_staged_fn : agg_ts_sample_fn, &ctx, AGG_TS_SAMPLE_TASKS);
    else
        (rr->staged ? agg_ts_sample_staged_fn : agg_ts_sample_fn)(&ctx, 0, 0, AGG_TS_SAMPLE_TASKS);
    if (rr->staged && (*err = ray_scan_fail_finish(rr->fail, par ? pool : NULL)) != NULL) {
        scratch_free(hdr);
        return (double)n_in;
    }
    /* Merge the slices' counts into one set. */
    int64_t d = 0, n = 0;
    for (int t = 0; t < AGG_TS_SAMPLE_TASKS; t++) {
        n += ns[t];
        const uint64_t* th = hs + (size_t)t * AGG_TS_SAMPLE_SETCAP;
        const uint16_t* tc = cnt + (size_t)t * AGG_TS_SAMPLE_SETCAP;
        for (int i = 0; i < AGG_TS_SAMPLE_SETCAP; i++) {
            uint64_t h = th[i];
            if (!h) continue;
            uint64_t at = h & (CAP - 1);
            while (ghs[at] && ghs[at] != h) at = (at + 1) & (CAP - 1);
            if (!ghs[at]) { ghs[at] = h; d++; }
            gcnt[at] = (uint16_t)(gcnt[at] + tc[i]);
        }
    }
    int64_t f1 = 0, f2 = 0;
    for (int64_t i = 0; i < CAP; i++) {
        if (gcnt[i] == 1) f1++; else if (gcnt[i] == 2) f2++;
    }
    scratch_free(hdr);
    if (n <= 0) return (double)n_in;
    *unseen_pct = (int)(f1 * 100 / n);
    if (d == n) return 9.2e18;
    return (double)d + (f2 > 0 ? (double)f1 * (double)f1 / (2.0 * (double)f2)
                                : (double)f1 * (double)(f1 - 1) / 2.0);
}

/* Declines (returns NULL, nothing recorded or allocated) when the sample
 * estimate says the radix scatter is the better fit; see the header. */
static ray_t* exec_group_v2_parallel_tablestack(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms, const agg_rr_t* krr,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
        int64_t group_limit, const ray_group_emit_filter_t* efp) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = ray_pool_total_workers(pool);

    /* Near-unique keys (the common radix shape) decline here, before any
     * allocation: the radix path then runs exactly as it always has. */
    ray_t* perr = NULL;
    if (agg_ts_precheck_declines(krr, key_cols, n_keys, nrows, sel, sel_prefix, n_sel, &perr)) {
        ray_profile_tick("tablestack: gate declined");
        return NULL;
    }
    if (perr) return perr;

    agg_desc_t d;
    ray_t* derr = NULL;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols, &derr)) return derr ? derr : ray_error("oom", NULL);

    int64_t n_in = sel ? n_sel : nrows;
    if (n_in < 0) n_in = 0;
    /* Logical tasks: a few per worker, never fewer rows than a table is
     * worth; the task -> range map depends on (rows, workers) only. */
    uint64_t want = (uint64_t)n_in / AGG_TS_TASK_ROWS;
    uint64_t max_tasks = nw > 1 ? (uint64_t)nw * AGG_TS_TASKS_PER_WORKER : 1;
    if (want > max_tasks) want = max_tasks;
    if (want < 1) want = 1;
    uint32_t n_tasks = (uint32_t)want;
    int64_t task_rows = (n_in + n_tasks - 1) / n_tasks;
    if (task_rows < 1) task_rows = 1;

    /* Gate + initial table size from one sample of the grouped rows (the
     * selected rows under a pushed filter). */
    size_t state_off = AGG_TS_KEY_OFF + (size_t)n_keys * sizeof(int64_t);
    size_t estride = (state_off + block + 7u) & ~(size_t)7u;
    int unseen_pct = 0;
    double est = agg_ts_estimate(pool, &d.rr, key_cols, n_keys, nrows,
                                 sel, sel_prefix, n_sel, &unseen_pct, &derr);
    if (derr) { agg_desc_free(&d); return derr; }
    uint32_t init_bits = AGG_TS_MIN_BITS;
    while (init_bits < AGG_TS_MAX_BITS &&
           (double)((int64_t)1 << init_bits) * 0.75 < est * 1.25) init_bits++;
    /* Every worker probes its own table at random: the entries the tables
     * in flight touch (plus their tag arrays) must stay in the last-level
     * cache together or each probe misses to DRAM (the same bound the dense
     * task-local slabs use; an unreported cache assumes 32 MB).  Wide
     * aggregate states shrink the admitted group count accordingly. */
    uint64_t llc_bytes = ray_cache_llc_bytes();
    double cache_budget = (llc_bytes ? (double)llc_bytes : 32.0 * 1048576.0) * 0.75;
    double table_bytes = est * (double)estride + (double)((int64_t)1 << init_bits);
    /* Chao1 under-estimates a long tail behind a repeated head (a few hot
     * keys supply the pairs its formula divides by while the tail stays
     * unseen): when more than AGG_TS_GATE_UNSEEN_PCT of the sampled rows
     * carry a key the sample saw once, most rows would open a new entry in
     * their task's table whatever the estimate says — decline. */
    if (est > (double)AGG_TS_GATE_GROUPS || est * AGG_TS_GATE_REPEAT > (double)task_rows ||
            unseen_pct > AGG_TS_GATE_UNSEEN_PCT ||
            table_bytes * (double)nw > cache_budget) {
        agg_desc_free(&d);
        ray_profile_tick("tablestack: gate declined");
        return NULL;
    }
    agg_route_record(AGG_ROUTE_V2_TABLESTACK);

    /* Record layout (scatter) + entry layout (tables), one carve. */
    ray_t* off_hdr;
    size_t* val_off = (size_t*)scratch_calloc(&off_hdr,
        3u * (size_t)(n_aggs ? n_aggs : 1) * sizeof(size_t));
    if (!val_off) { agg_desc_free(&d); return ray_error("oom", NULL); }
    size_t* val2_off = val_off + (n_aggs ? n_aggs : 1);
    size_t* ent_off  = val2_off + (n_aggs ? n_aggs : 1);
    size_t rec_cur = (size_t)n_keys * 8;
    for (uint32_t a = 0; a < n_aggs; a++) {
        if (d.val_id[a])  { val_off[a]  = rec_cur; rec_cur += d.val_esz[a]; }
        if (d.val2_id[a]) { val2_off[a] = rec_cur; rec_cur += d.val2_esz[a]; }
    }
    size_t row_off = rec_cur; rec_cur += 8;
    size_t rec = (rec_cur + 7u) & ~(size_t)7u;
    for (uint32_t a = 0; a < n_aggs; a++) ent_off[a] = state_off + off[a];

    /* Merge partitions: enough for the pool, a power of two no larger than
     * the smallest table (its slot range per partition must be >= 1). */
    uint32_t n_parts = 64;
    while (n_parts < 8u * nw && n_parts < 1024u) n_parts <<= 1;

    agg_ts_task_t* tasks = ray_calloc_raw((size_t)n_tasks * sizeof(agg_ts_task_t));
    agg_radix_part_t* parts = ray_calloc_raw((size_t)n_parts * sizeof(agg_radix_part_t));
    bool alloc_ok = tasks && parts;
    /* Each task's first table is carved HERE, on the calling thread that
     * also frees it after the merge: an allocation made by a worker and
     * freed by another thread is parked on the owner's foreign list, and
     * tens of such tables per query would shift the heap the next query
     * runs on.  Growth and pushed tables (rarer) still come from workers. */
    for (uint32_t t = 0; alloc_ok && t < n_tasks; t++) {
        tasks[t].top = agg_ts_table_new(init_bits, estride);
        if (!tasks[t].top) alloc_ok = false; else tasks[t].n_tables = 1;
    }
    if (!alloc_ok) {
        if (tasks) for (uint32_t t = 0; t < n_tasks; t++) agg_ts_table_free(tasks[t].top);
        ray_free_raw(tasks); ray_free_raw(parts);
        scratch_free(off_hdr); agg_desc_free(&d);
        return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
    }

    agg_ts_ctx_t ctx = {
        .key_cols = key_cols, .key_data = d.key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = d.val_id, .val_types = d.val_types, .val_hasnull = d.val_hasnull, .val_esz = d.val_esz,
        .val2_data = d.val2_id, .val2_types = d.val2_types, .val2_hasnull = d.val2_hasnull, .val2_esz = d.val2_esz,
        .vd = { .n_aggs = n_aggs, .vts = vts, .off = ent_off, .block = estride,
                .val_data = d.val_id, .val_types = d.val_types, .val_hasnull = d.val_hasnull, .val_esz = d.val_esz,
                .val2_data = d.val2_id, .val2_types = d.val2_types, .val2_hasnull = d.val2_hasnull, .val2_esz = d.val2_esz },
        .ent_off = ent_off, .estride = estride, .state_off = state_off,
        .rec = rec, .row_off = row_off, .val_off = val_off, .val2_off = val2_off,
        .n_parts = n_parts, .part_bits = (uint32_t)__builtin_ctz(n_parts),
        .init_bits = init_bits,
        .n_in = n_in, .task_rows = task_rows, .n_tasks = n_tasks,
        .tasks = tasks, .parts = parts,
        .sel = sel, .sel_prefix = sel_prefix,
        .rr = &d.rr, .whole = &d.whole,
    };

    agg_desc_advise(&d, nrows);
    ray_pool_dispatch_n(pool, agg_ts_phase1_fn, &ctx, n_tasks);
    ray_t* ferr = d.staged ? ray_scan_fail_finish(&d.fail, pool) : NULL;
    bool cancelled = pool_cancelled(pool) || ferr;
    int oom = 0;
    uint32_t scatter_tasks = 0; uint64_t tables = 0; int64_t cand = 0;
    for (uint32_t t = 0; t < n_tasks; t++) {
        if (tasks[t].oom) oom = 1;
        if (tasks[t].scatter) scatter_tasks++;
        tables += tasks[t].n_tables;
        cand += tasks[t].entries + tasks[t].scattered;
    }
    ctx.merge_cand = cand;
    route_stats.ts_tasks = n_tasks;
    route_stats.ts_scatter_tasks = scatter_tasks;
    route_stats.ts_tables = tables;
    ray_profile_tick("tablestack: aggregated tasks");

    if (!cancelled && !oom) {
        ray_pool_dispatch_n(pool, agg_ts_merge_fn, &ctx, n_parts);
        cancelled = pool_cancelled(pool);
        for (uint32_t p = 0; p < n_parts; p++) if (parts[p].oom) { oom = 1; break; }
    }
    for (uint32_t t = 0; t < n_tasks; t++) agg_ts_task_free(&tasks[t], n_parts);
    ray_free_raw(tasks);
    scratch_free(off_hdr);
    ray_profile_tick("tablestack: merged partitions");

    if (cancelled || oom) {
        agg_radix_parts_destroy(parts, n_parts, vts, off, block, n_aggs);
        ray_free_raw(parts);
        agg_desc_free(&d);
        if (ferr) return ferr;
        if (cancelled) return ray_error("cancel", NULL);
        return exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
    }
    return agg_parts_emit(ext, &d, parts, n_parts, pool, nw, key_cols, key_syms,
                          vts, off, block, n_in, group_limit, efp);
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
                if (ex) agg_sel_gather_vals(x, ray_data(c->x), ex, c->rows + at, n, 0);
                if (ey) agg_sel_gather_vals(y, ray_data(c->y), ey, c->rows + at, n, 0);
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
            case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP:
                INDEX_FIRST_VALID(int64_t, value != NULL_I64);
            case RAY_F32: INDEX_FIRST_VALID(float, value == value);
            case RAY_F64: INDEX_FIRST_VALID(double, value == value);
            default: break;
        }
#undef INDEX_FIRST_VALID
    }
    int64_t best = -1;
    /* A FILE domain's entries are compared off the mapped vocabulary: no
     * atom per row — the lazily built atoms of a wide vocabulary cost far
     * more than the compare and stay for the domain's lifetime.  Positions
     * past the file prefix, and the runtime domain, go through the atoms. */
    struct ray_sym_domain_s* dom = ray_sym_vec_domain(c->src);
    ray_sym_domain_raw_t vocab;
    bool vocab_ok = ray_sym_domain_raw_pin(dom, &vocab);
    for (int64_t j = 0; j < count; j++) {
        if ((j & 65535) == 0 && ray_interrupted()) return -1;
        int64_t r = rows[c->kind == OP_LAST ? count - 1 - j : j];
        if (ray_vec_is_null(c->src, r)) continue;
        if (best < 0) best = r;
        if (c->kind == OP_FIRST || c->kind == OP_LAST) break;
        int64_t ir = ray_read_sym(data, r, c->src->type, c->src->attrs);
        int64_t ib = ray_read_sym(data, best, c->src->type, c->src->attrs);
        int cmp;
        if (vocab_ok && ir >= 0 && ib >= 0 && ir < vocab.count && ib < vocab.count) {
            size_t lr, lb;
            const char* pr = ray_sym_domain_raw_str(&vocab, ir, &lr);
            const char* pb = ray_sym_domain_raw_str(&vocab, ib, &lb);
            size_t m = lr < lb ? lr : lb;
            cmp = m ? memcmp(pr, pb, m) : 0;
            if (cmp == 0) cmp = (lr > lb) - (lr < lb);
        } else {
            ray_t* x = ray_group_sym_read(&c->symbols, dom, ir);
            ray_t* y = ray_group_sym_read(&c->symbols, dom, ib);
            cmp = ray_str_cmp(x, y);
        }
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
    agg_dense_plan_free(&dp);   /* compaction tables serve the key build only */
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
        ray_t* src = ie ? ray_table_col(tbl, ie->sym) : NULL;
        if (src && RAY_IS_ERR(src)) { ray_release(result); result = src; goto done; }
        int64_t param = ext->agg_k ? ext->agg_k[a] : 0;
        ray_t* col = NULL;
        /* The kernels below read src at every row, group by group, so at
         * scattered rows: a cold mapped column is requested first, whole
         * (scan read-ahead). */
        if (src && ray_pool_scan_on()) ray_col_want_rows(src, rows, nrows);
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
            ray_t* ycol = ray_table_col(tbl, ye->sym);
            if (ycol && RAY_IS_ERR(ycol)) { ray_release(result); result = ycol; goto done; }
            col = agg_index_streaming(agg_resolve(kind, src->type), src,
                ycol, &groups, rows, offsets, nrows, param);
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
        ray_t** keys, const agg_vo_t* vo, int64_t rows, ray_t** err) {
    *err = NULL;
    if (!ext->n_aggs) return true;
    if (rows < 1024) return false;
    enum { SAMPLES = 1024, CAP = 2048 };
    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, keys, err)) return false;   /* *err: failed bind; else OOM = not shared */
    char* states = ray_alloc_raw((SAMPLES + 1) * vo->block);
    if (!states) { agg_desc_free(&d); return false; }
    /* Staged sources: strided single rows, ascending, through cursors
     * (R-SPARSE). */
    agg_curs_t cs; cs.cur = NULL;
    if (d.staged && !agg_curs_init(&cs, &d.rr)) { ray_free_raw(states); agg_desc_free(&d); return false; }
    char* previous = states + SAMPLES * vo->block;
    int16_t map[CAP]; int64_t values[SAMPLES];
    memset(map, -1, sizeof(map));
    uint32_t gid = 0;
    int groups = 0, unchanged = 0;
    int64_t step = rows / SAMPLES;
    if (!d.staged && ray_col_cold(keys[0]))
        for (int i = 0; i < SAMPLES; i++)
            ray_col_want_row(keys[0], i * step + (int64_t)(ray_hash_i64(i) % (uint64_t)step));
    for (int i = 0; i < SAMPLES && !*err; i++) {
        uint64_t jitter = ray_hash_i64(i);
        int64_t row = i * step + jitter % step;
        int64_t value = agg_key_at(&d.rr, &cs, 0, row, err);
        if (*err) break;
        uint32_t at = ray_hash_i64(value) & (CAP - 1);
        while (map[at] >= 0 && values[map[at]] != value) at = (at + 1) & (CAP - 1);
        if (map[at] < 0) {
            map[at] = groups; values[groups] = value;
            for (uint32_t a = 0; a < ext->n_aggs; a++) vo->vts[a]->init(states + groups * vo->block + vo->off[a]);
            groups++;
        }
        char* state = states + map[at] * vo->block;
        memcpy(previous, state, vo->block);
        for (uint32_t a = 0; a < ext->n_aggs && !*err; a++) {
            const void* input = agg_elem_at(&d.rr, &cs, ext->n_keys + a, row, err);
            ray_valid_t valid = {input, d.val_types[a], d.val_hasnull[a]};
            if (!*err) vo->vts[a]->update_shared(state + vo->off[a], vo->block, &gid, input, &valid, 1);
        }
        unchanged += memcmp(previous, state, vo->block) == 0;
    }
    agg_curs_free(&cs, &d.rr);
    ray_free_raw(states); agg_desc_free(&d);
    return !*err && unchanged >= SAMPLES / 4;
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

/* A segment-streamed table (g->grp_seg_stream) holding a parted column. */
static bool agg_seg_parted(ray_graph_t* g, ray_t* tbl) {
    if (!g->grp_seg_stream) return false;
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_col_meta_t m;
        if (ray_table_col_meta_idx(tbl, c, &m) && RAY_IS_PARTED(m.type)) return true;
    }
    return false;
}

/* R-RANDOM over a segment-streamed table: a strategy that reads keys or
 * inputs at stored rows or whole runs over a flat copy of the table, each
 * parted column copied from its segments by range (parted_flatten_fixed,
 * the concat fallback's own copy), released when the run returns. */
static ray_t* agg_seg_flat_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                               int64_t nrows, ray_t* sel,
                               const int64_t* sel_prefix, int64_t n_sel,
                               int64_t group_limit,
                               const ray_group_emit_filter_t* efp) {
    int64_t nc = ray_table_ncols(tbl);
    int64_t fx_ncnt = 0;
    const int64_t* fx_cnt = parted_table_counts(tbl, &fx_ncnt);
    ray_t* flat = ray_table_new(nc > 0 ? nc : 1);
    for (int64_t c = 0; c < nc && flat && !RAY_IS_ERR(flat); c++) {
        ray_t* slot = ray_table_slot_idx(tbl, c);
        ray_col_meta_t m;
        ray_t* v = ray_col_meta_of(slot, &m) && RAY_IS_PARTED(m.type) ? parted_flatten_fixed_c(slot, nrows, fx_cnt, fx_ncnt) : slot;
        if (!v || RAY_IS_ERR(v)) { ray_release(flat); return v ? v : ray_error("oom", NULL); }
        flat = ray_table_add_col(flat, ray_table_col_name(tbl, c), v);
        if (v != slot) ray_release(v);
    }
    if (!flat || RAY_IS_ERR(flat)) return flat ? flat : ray_error("oom", NULL);
    ray_t* r = exec_group_v2_run_inner(g, op, flat, nrows, sel, sel_prefix, n_sel, group_limit, efp);
    ray_graph_unbind_table(g, flat);
    ray_release(flat);
    return r;
}

/* Trim a full group result to the emit filter's keep set (the same decision
 * the native selections make, over the finished aggregate column).  Row
 * order is preserved.  Consumes `result`; a failure keeps the full result,
 * which is still a valid answer for the sort+take downstream. */
ray_t* agg_emit_filter_trim(ray_t* result, uint32_t n_keys, uint32_t n_aggs,
                            const uint16_t* agg_ops, const ray_group_emit_filter_t* ef) {
    if (!result || RAY_IS_ERR(result) || result->type != RAY_TABLE) return result;
    if (ef->agg_index >= n_aggs) return result;
    /* second line of defence: never trim by a slot the filter did not name */
    if (agg_ops && (ef->agg_op ? ef->agg_op : OP_COUNT) != agg_ops[ef->agg_index]) return result;
    int64_t nrows = ray_table_nrows(result);
    if (nrows <= 0) return result;
    ray_t* vcol = ray_table_col_idx(result, (int64_t)n_keys + ef->agg_index);
    if (vcol && RAY_IS_ERR(vcol)) { ray_error_free(vcol); return result; }   /* the sort+take downstream re-reads it */
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
/* True when the thread-local emit filter was armed for THIS node: the slot
 * exists and holds the operation the filter names (count when unset).  The
 * filter stays armed while the matched select's whole `from:` evaluates, so
 * grouped selects nested inside see it too; for them this is false. */
bool agg_emit_filter_targets(const ray_group_emit_filter_t* ef, const ray_op_ext_t* ext) {
    return ef && ef->enabled && ext && ef->agg_index < ext->n_aggs &&
           (ef->agg_op ? ef->agg_op : OP_COUNT) == ext->agg_ops[ef->agg_index];
}

static ray_t* exec_group_v2_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit,
                                const ray_group_emit_filter_t* efp) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!agg_emit_filter_targets(efp, ext)) efp = NULL;   /* one decision for routing AND trim */
    ray_t* r = exec_group_v2_run_inner(g, op, tbl, nrows, sel, sel_prefix, n_sel,
                                       group_limit, efp);
    if (efp && r && !RAY_IS_ERR(r) && !route_stats.topn_native)
        r = agg_emit_filter_trim(r, ext->n_keys, ext->n_aggs, ext->agg_ops, efp);
    return r;
}

/* ══════════════════════════════════════════════════════════════════════
 * First-N groups for an unordered `take: N`.
 *
 * With a HEAD(GROUP) limit hint and an input large enough for the parallel
 * strategies, every route exec_group_v2_run_inner can take for an int/SYM-
 * keyed streaming group emits the groups in first-seen order and keeps the N
 * with the smallest first row (task-local dense selects them in
 * agg_dense_finish, radix in agg_radix_select_first_n or its first-seen
 * restore; the shared and partitioned dense strategies are gated off by the
 * limit).  Those N groups are the first N distinct key tuples in (selected)
 * row order, so they can be found before aggregating anything:
 *
 *   1. a serial scan from the first selected row collects the first N
 *      distinct key tuples into a small hash set;
 *   2. a parallel pass collects, in row order, every selected row whose key
 *      is in the set, and the unchanged strategy dispatch aggregates just
 *      those rows (a compact table of the keys and aggregate inputs), so each
 *      aggregate covers ALL rows of its group;
 *   3. the re-run gets the same limit (all N groups), so its routes keep
 *      first-seen order too; the order is still checked against the set's
 *      ranks and put right if a route ever disagrees.
 *
 * Admitted aggregates are the ones whose value does not depend on the order
 * rows are folded in: count of any input, and sum/avg/min/max over integer
 * and temporal inputs (a float sum regroups the additions).  The pre-pass
 * gives up — the caller runs the full aggregation and only the probe is
 * spent — when the N keys are not found within a bounded prefix, or when the
 * N groups hold a large share of the rows (sampled before the parallel pass,
 * counted exactly in it), where aggregating everything costs about the same. */
#define AGG_FIRSTN_SAMPLES    4096     /* evenly spaced rows for the share estimate */
#define AGG_FIRSTN_SHARE      64       /* full run once the N groups hold 1/64 of the rows */

bool ray_agg_first_n = true;
static _Thread_local bool firstn_nested;   /* inside the pre-pass's own group run */

typedef struct {
    int64_t*  keys;     /* cap * n_keys */
    int32_t*  rank;     /* cap; -1 marks an empty slot */
    uint64_t* bloom;    /* 1 << bbits bits over the members' hashes */
    uint32_t  n_keys;
    uint32_t  bits;     /* cap = 1 << bits */
    uint32_t  bbits;
    int64_t   n;        /* tuples inserted; rank = insertion order */
} agg_firstn_set_t;

/* Tuple hash, folded one key at a time so a morsel can be hashed one key
 * COLUMN at a time (agg_firstn_hash_block) with the same result. */
#define AGG_FIRSTN_SEED 0x9E3779B97F4A7C15ULL
#define AGG_FIRSTN_MIX(h, v) \
    do { (h) = ((h) ^ (uint64_t)(v)) * 0x9E3779B97F4A7C15ULL; (h) ^= (h) >> 32; } while (0)
#define AGG_FIRSTN_FINAL(h) ((h) * 0xD6E8FEB86659FD93ULL)

static inline uint64_t agg_firstn_hash(const int64_t* kv, uint32_t n_keys) {
    uint64_t h = AGG_FIRSTN_SEED;
    for (uint32_t k = 0; k < n_keys; k++) AGG_FIRSTN_MIX(h, kv[k]);
    return AGG_FIRSTN_FINAL(h);
}

/* Rank of the tuple (hash `hv`), or -1 when absent.  With `insert`, an absent
 * tuple is added with the next rank (the caller keeps the set a quarter full). */
static inline int64_t agg_firstn_probe(agg_firstn_set_t* s, const int64_t* kv,
                                       uint64_t hv, bool insert) {
    uint64_t mask = ((uint64_t)1 << s->bits) - 1;
    for (uint64_t i = hv >> (64 - s->bits);; i = (i + 1) & mask) {
        int32_t r = s->rank[i];
        int64_t* sk = s->keys + i * s->n_keys;
        if (r < 0) {
            if (!insert) return -1;
            for (uint32_t k = 0; k < s->n_keys; k++) sk[k] = kv[k];
            s->rank[i] = (int32_t)s->n;
            s->n++;
            return -1;
        }
        uint32_t k = 0;
        while (k < s->n_keys && sk[k] == kv[k]) k++;
        if (k == s->n_keys) return r;
    }
}

/* Fold key column `col` rows [b, b+n) into the running hashes h[0..n). The
 * widening matches agg_read_key_i64 (signed types sign-extend, SYM and U8/BOOL
 * zero-extend), so the folded hash equals agg_firstn_hash of the read tuple. */
/* Always inlined: the per-task hash pass is the pre-pass's hot loop, and a
 * second caller (the pick) must not turn it into a call. */
static inline __attribute__((always_inline)) void agg_firstn_hash_block(uint64_t* h, ray_t* col, const void* data,
                                  int64_t b, int64_t n) {
    switch (col->type) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* d = (const int64_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], d[i]);
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* d = (const int32_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
            break;
        }
        case RAY_I16: {
            const int16_t* d = (const int16_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
            break;
        }
        case RAY_U8: case RAY_BOOL: {
            const uint8_t* d = (const uint8_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
            break;
        }
        case RAY_SYM:
            switch (col->attrs & RAY_SYM_W_MASK) {
                case RAY_SYM_W8: {
                    const uint8_t* d = (const uint8_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
                    break;
                }
                case RAY_SYM_W16: {
                    const uint16_t* d = (const uint16_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
                    break;
                }
                case RAY_SYM_W32: {
                    const uint32_t* d = (const uint32_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
                    break;
                }
                default: {
                    const int64_t* d = (const int64_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], d[i]);
                    break;
                }
            }
            break;
        default:   /* admission keeps every other type out */
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], agg_read_key_i64(col, data, b + i));
            break;
    }
}
/* agg_firstn_hash_block over a type and SYM width instead of a column:
 * the staged passes' instance (a view's width is per piece), kept apart so
 * the plain passes keep their code. */
static inline __attribute__((always_inline)) void agg_firstn_hash_raw(uint64_t* h, int8_t type, uint8_t attrs,
                                  const void* data, int64_t b, int64_t n) {
    switch (type) {
        case RAY_TIMESPAN: case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* d = (const int64_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], d[i]);
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* d = (const int32_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
            break;
        }
        case RAY_I16: {
            const int16_t* d = (const int16_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
            break;
        }
        case RAY_U8: case RAY_BOOL: {
            const uint8_t* d = (const uint8_t*)data + b;
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
            break;
        }
        case RAY_SYM:
            switch (attrs & RAY_SYM_W_MASK) {
                case RAY_SYM_W8: {
                    const uint8_t* d = (const uint8_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
                    break;
                }
                case RAY_SYM_W16: {
                    const uint16_t* d = (const uint16_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
                    break;
                }
                case RAY_SYM_W32: {
                    const uint32_t* d = (const uint32_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], (int64_t)d[i]);
                    break;
                }
                default: {
                    const int64_t* d = (const int64_t*)data + b;
                    for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], d[i]);
                    break;
                }
            }
            break;
        default:   /* admission keeps every other type out */
            for (int64_t i = 0; i < n; i++) AGG_FIRSTN_MIX(h[i], agg_read_key_i64_raw(type, attrs, data, b + i));
            break;
    }
}

#define AGG_FIRSTN_LOCAL 1024          /* matches a task keeps before spilling */

typedef struct {
    agg_firstn_set_t* set;
    ray_t**      key_cols;
    const void** key_data;
    uint32_t     n_keys;
    int64_t      nrows;
    ray_t*       sel;          /* rowsel or NULL */
    uint32_t     n_segs, tasks;
    int64_t*     counts;       /* per task: matches; then the write offset */
    int64_t*     local;        /* tasks * AGG_FIRSTN_LOCAL: the first matches */
    int64_t*     out;          /* write pass: the final row list */
    int64_t*     kvs;          /* tasks * n_keys: each task's probe tuple */
    const agg_rr_t* rr;        /* staged keys (agg_firstn_task_staged); NULL: plain */
    const int64_t*  mem_v;     /* staged: n_keys member sets of 1 << mem_bits slots */
    const uint8_t*  mem_used;
    uint32_t        mem_bits;
} agg_firstn_ctx_t;

/* One task: the selected rows of its morsel segments whose key tuple is in
 * the set, in row order.  Each morsel is hashed one key column at a time; the
 * bloom bit rejects almost every row before any per-row key read. */
static void agg_firstn_task(agg_firstn_ctx_t* c, uint32_t t) {
    int64_t s0 = (int64_t)c->n_segs * t / c->tasks;
    int64_t s1 = (int64_t)c->n_segs * (t + 1) / c->tasks;
    agg_firstn_set_t* set = c->set;
    uint32_t n_keys = c->n_keys;
    uint64_t hb[RAY_MORSEL_ELEMS];
    int64_t* kv = c->kvs + (int64_t)t * n_keys;
    const uint8_t*  fl = c->sel ? ray_rowsel_flags(c->sel) : NULL;
    const uint32_t* of = c->sel ? ray_rowsel_offsets(c->sel) : NULL;
    const uint16_t* ix = c->sel ? ray_rowsel_idx(c->sel) : NULL;
    int64_t* out = c->out ? c->out + c->counts[t] : c->local + (int64_t)t * AGG_FIRSTN_LOCAL;
    int64_t out_cap = c->out ? INT64_MAX : AGG_FIRSTN_LOCAL;
    uint32_t bshift = 64 - set->bbits;
    int64_t n = 0;
    for (int64_t seg = s0; seg < s1; seg++) {
        if (fl && fl[seg] == RAY_SEL_NONE) continue;
        int64_t b = seg * RAY_MORSEL_ELEMS;
        int64_t len = c->nrows - b < RAY_MORSEL_ELEMS ? c->nrows - b : RAY_MORSEL_ELEMS;
        if (!(seg & 7)) ray_pool_scan_at(c->nrows, b, s1 * RAY_MORSEL_ELEMS);
        for (int64_t i = 0; i < len; i++) hb[i] = AGG_FIRSTN_SEED;
        for (uint32_t k = 0; k < n_keys; k++)
            agg_firstn_hash_block(hb, c->key_cols[k], c->key_data[k], b, len);
        bool mix = fl && fl[seg] == RAY_SEL_MIX;
        int64_t j_end = mix ? (int64_t)of[seg + 1] : len;
        for (int64_t j = mix ? (int64_t)of[seg] : 0; j < j_end; j++) {
            int64_t i = mix ? (int64_t)ix[j] : j;
            uint64_t hv = AGG_FIRSTN_FINAL(hb[i]);
            uint64_t bit = hv >> bshift;
            if (!((set->bloom[bit >> 6] >> (bit & 63)) & 1)) continue;
            for (uint32_t k = 0; k < n_keys; k++)
                kv[k] = agg_read_key_i64(c->key_cols[k], c->key_data[k], b + i);
            if (agg_firstn_probe(set, kv, hv, false) < 0) continue;
            if (n < out_cap) out[n] = b + i;
            n++;
        }
    }
    if (!c->out) c->counts[t] = n;
}

static void agg_firstn_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    for (int64_t t = start; t < end; t++) agg_firstn_task((agg_firstn_ctx_t*)raw, (uint32_t)t);
}

/* ---- The passes over staged keys (generation 2, single file or parted):
 * the keys are read through the key set's sources, never unstaged.  Rows
 * are read in order through task-held cursors (each touched vector decoded
 * once per task); the share sample point-reads its rows.  Separate
 * instances: the plain passes above keep their code. ---- */

/* Key k at row r through the cursor set: in place for a plain key, else
 * from its cursor's view.  *err set on a range error. */
static inline int64_t agg_firstn_key_cur(const agg_rr_t* rr, agg_curs_t* cs, uint32_t k, int64_t r, ray_t** err) {
    if (!rr->srcs[k]) return agg_read_key_i64_raw(rr->key_types[k], rr->key_attrs[k], rr->plain[k], r);
    const ray_col_range_t* v;
    if ((*err = ray_col_cursor_at(&cs->cur[k], r, &v)) != NULL) return 0;
    return agg_read_key_i64_raw(rr->key_types[k], v->attrs, v->data, r - v->start);
}

/* Pass 1 over staged keys: the first group_limit distinct tuples from the
 * first selected row, within the row budget, through one cursor set.  NULL
 * or an owned error. */
static ray_t* agg_firstn_seen_staged(const agg_rr_t* rr, agg_firstn_set_t* set, ray_t* sel,
                                     int64_t nrows, int64_t n_segs, int64_t group_limit, int64_t* kv) {
    agg_curs_t cs;
    if (!agg_curs_init(&cs, rr)) return ray_error("oom", NULL);
    const uint8_t*  fl = sel ? ray_rowsel_flags(sel) : NULL;
    const uint32_t* of = sel ? ray_rowsel_offsets(sel) : NULL;
    const uint16_t* ix = sel ? ray_rowsel_idx(sel) : NULL;
    int64_t budget = group_limit * 64 > AGG_FIRSTN_PROBE_ROWS ? group_limit * 64 : AGG_FIRSTN_PROBE_ROWS;
    int64_t scanned = 0;
    ray_t* err = NULL;
    for (int64_t seg = 0; seg < n_segs && set->n < group_limit && scanned < budget && !err; seg++) {
        if (fl && fl[seg] == RAY_SEL_NONE) continue;
        int64_t b = seg * RAY_MORSEL_ELEMS;
        int64_t len = nrows - b < RAY_MORSEL_ELEMS ? nrows - b : RAY_MORSEL_ELEMS;
        bool mix = fl && fl[seg] == RAY_SEL_MIX;
        int64_t j_end = mix ? (int64_t)of[seg + 1] : len;
        for (int64_t j = mix ? (int64_t)of[seg] : 0;
             j < j_end && set->n < group_limit && scanned < budget; j++, scanned++) {
            int64_t row = b + (mix ? (int64_t)ix[j] : j);
            for (uint32_t k = 0; k < rr->n_keys && !err; k++) kv[k] = agg_firstn_key_cur(rr, &cs, k, row, &err);
            if (err) break;
            agg_firstn_probe(set, kv, agg_firstn_hash(kv, rr->n_keys), true);
        }
    }
    agg_curs_free(&cs, rr);
    return err;
}

/* The share sample over staged keys: the plain sample's rows, each key
 * point-read (agg_key_at: no vector decoded; SYM ids too) or, declined (RAW,
 * DELTA_BP, nulls), read through a cursor, which decodes the sampled
 * vector.  The rows are split across pool tasks when the pass-2 dispatch
 * is (each task its own cursors), so declined reads do not decode serially;
 * a range error is noted keyed by the task's first sample row. */
typedef struct {
    const agg_rr_t*   rr;
    agg_firstn_set_t* set;
    int64_t           step, samples;
    uint32_t          tasks;
    int64_t*          hits;      /* [tasks] */
    int64_t*          kvs;       /* [tasks * n_keys] */
} agg_firstn_share_t;

static void agg_firstn_share_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_firstn_share_t* c = (agg_firstn_share_t*)raw;
    const agg_rr_t* rr = c->rr;
    for (int64_t t = start; t < end; t++) {
        int64_t i0 = c->samples * t / c->tasks, i1 = c->samples * (t + 1) / c->tasks;
        int64_t key = i0 * c->step, hits = 0;
        int64_t* kv = c->kvs + t * rr->n_keys;
        c->hits[t] = 0;
        if (i0 >= i1 || ray_scan_fail_skip(rr->fail, key)) continue;
        agg_curs_t cs;
        if (!agg_curs_init(&cs, rr)) { ray_scan_fail_note(rr->fail, key, ray_error("oom", NULL)); continue; }
        ray_t* err = NULL;
        for (int64_t i = i0; i < i1 && !err; i++) {
            for (uint32_t k = 0; k < rr->n_keys && !err; k++) kv[k] = agg_key_at(rr, &cs, k, i * c->step, &err);
            if (!err && agg_firstn_probe(c->set, kv, agg_firstn_hash(kv, rr->n_keys), false) >= 0) hits++;
        }
        agg_curs_free(&cs, rr);
        if (err) ray_scan_fail_note(rr->fail, key, err);
        c->hits[t] = hits;
    }
}

/* NULL or an owned error; *hits the sampled rows in the set. */
static ray_t* agg_firstn_share_staged(const agg_rr_t* rr, agg_firstn_set_t* set, int64_t nrows,
                                      int64_t samples, ray_pool_t* pool, bool par, int64_t* hits) {
    uint32_t tasks = par ? ray_pool_total_workers(pool) : 1;
    if ((int64_t)tasks > samples) tasks = (uint32_t)(samples > 0 ? samples : 1);
    ray_t* hdr = NULL;
    int64_t* buf = (int64_t*)scratch_alloc(&hdr, (size_t)tasks * (1 + rr->n_keys) * sizeof(int64_t));
    if (!buf) return ray_error("oom", NULL);
    agg_firstn_share_t c = { .rr = rr, .set = set, .step = nrows / samples, .samples = samples,
                             .tasks = tasks, .hits = buf, .kvs = buf + tasks };
    ray_scan_fail_init(rr->fail);
    if (tasks > 1) ray_pool_dispatch_n(pool, agg_firstn_share_fn, &c, tasks);
    else agg_firstn_share_fn(&c, 0, 0, 1);
    ray_t* err = ray_scan_fail_finish(rr->fail, tasks > 1 ? pool : NULL);
    *hits = 0;
    for (uint32_t t = 0; t < tasks; t++) *hits += buf[t];
    scratch_free(hdr);
    return err;
}

/* One piece of a staged pass-2 segment: rows [b, e) of every key (a piece
 * ends at the nearest view end), hashed a key at a time, then the
 * segment's selected rows inside it probed. */
typedef struct {
    agg_firstn_ctx_t* c;
    uint64_t*         hb;       /* RAY_MORSEL_ELEMS */
    int64_t*          kv;
    int64_t*          out;
    int64_t           out_cap, n;
    int64_t           seg_b;    /* the segment's first row */
    const uint16_t*   ix;       /* MIX: selected indices [j, j_end) of the segment */
    int64_t           j, j_end;
} agg_firstn_piece_t;

static void agg_firstn_piece(void* ctx, uint32_t wid, int64_t b, int64_t e, const ray_col_piece_t* p) {
    (void)wid;
    agg_firstn_piece_t* t = (agg_firstn_piece_t*)ctx;
    const agg_rr_t* rr = t->c->rr;
    agg_firstn_set_t* set = t->c->set;
    const uint32_t nk = rr->n_keys;
    const void* kd[nk];
    uint8_t ka[nk];
    const int64_t n = e - b;
    for (int64_t i = 0; i < n; i++) t->hb[i] = AGG_FIRSTN_SEED;
    for (uint32_t k = 0; k < nk; k++) {
        kd[k] = rr->srcs[k] ? p[k].data : (const char*)rr->plain[k] + (size_t)b * rr->esz[k];
        ka[k] = rr->srcs[k] ? p[k].attrs : rr->key_attrs[k];
        agg_firstn_hash_raw(t->hb, rr->key_types[k], ka[k], kd[k], 0, n);
    }
    const uint32_t bshift = 64 - set->bbits;
    for (int64_t q = t->ix ? t->j : 0; q < (t->ix ? t->j_end : n); q++) {
        int64_t i;
        if (t->ix) {
            int64_t r = t->seg_b + (int64_t)t->ix[q];
            if (r >= e) break;                 /* the next piece's */
            i = r - b;
            t->j = q + 1;
        } else {
            i = q;
        }
        uint64_t hv = AGG_FIRSTN_FINAL(t->hb[i]);
        uint64_t bit = hv >> bshift;
        if (!((set->bloom[bit >> 6] >> (bit & 63)) & 1)) continue;
        for (uint32_t k = 0; k < nk; k++) t->kv[k] = agg_read_key_i64_raw(rr->key_types[k], ka[k], kd[k], i);
        if (agg_firstn_probe(set, t->kv, hv, false) < 0) continue;
        if (t->n < t->out_cap) t->out[t->n] = b + i;
        t->n++;
    }
}

/* Lane i of a view's run lanes at width T. */
static inline uint64_t agg_vv_lane(const ray_enc_lanes_t* l, int T, uint32_t i) {
    switch (T) {
    case 8:  return l->b[i];
    case 16: return l->h[i];
    case 32: return l->s[i];
    default: return l->q[i];
    }
}

/* Each key's member values (the distinct values key k takes among the N
 * groups' tuples), an exact open-addressing set: pass 2 on runs skips a
 * run whose value no group holds without reading the other key. */
static inline uint32_t agg_firstn_mslot(int64_t x, uint32_t bits) {
    return (uint32_t)(((uint64_t)x * 0x9E3779B97F4A7C15ULL) >> (64 - bits));
}
static inline bool agg_firstn_member(const agg_firstn_ctx_t* c, uint32_t k, int64_t x) {
    const uint32_t cap = 1u << c->mem_bits, mask = cap - 1;
    const int64_t* v = c->mem_v + (size_t)k * cap;
    const uint8_t* u = c->mem_used + (size_t)k * cap;
    for (uint32_t i = agg_firstn_mslot(x, c->mem_bits);; i = (i + 1) & mask) {
        if (!u[i]) return false;
        if (v[i] == x) return true;
    }
}

/* Key k's vector at segment `seg` when it is a run view (RLE or CONST, not a float,
 * without a null bitmap: SYM nulls are in-band ids), read by
 * ray_col_src_vview, which makes every check the decode would: 1, 0 not
 * one (read the vector otherwise), -1 with *err.  The descriptor's
 * encoding is looked at first, so no other codec builds a view. */
static int agg_firstn_runview(const agg_rr_t* rr, uint32_t k, int64_t seg, int64_t len,
                              ray_enc_vview_t* vv, ray_t** err) {
    ray_col_src_t* src = rr->srcs[k];
    if (!src || !ray_col_src_encoded(src) || ray_enccol_decoded_peek(src->obj)) return 0;
    ray_enccol_t* e = ray_enccol_of(src->obj);
    if (e->kind != RAY_ENCCOL_SINGLE) return 0;
    /* a float view's run values are bit patterns: compared as integers they
     * would split -0.0 from +0.0, so a float key is read by its values */
    if (e->type == RAY_F32 || e->type == RAY_F64) return 0;
    uint8_t enc = ray_enccol_descs(e, seg / RAY_ENC_VECS_PER_CHUNK)[seg % RAY_ENC_VECS_PER_CHUNK].encoding;
    if (enc != RAY_ENC_RLE && enc != RAY_ENC_CONST) return 0;
    int r = ray_col_src_vview(src, seg, vv, err);
    if (r <= 0) return r;
    if ((vv->enc != RAY_ENC_RLE && vv->enc != RAY_ENC_CONST) || vv->n != len ||
        vv->nullmap || (vv->n_nulls && rr->key_types[k] != RAY_SYM)) return 0;
    return 1;
}

/* A key's reader inside one segment: its runs (a run view, the current
 * run tracked) or its values (the cursor's decoded view, or in place). */
typedef struct {
    bool            runs;
    ray_enc_vview_t* vv;
    uint32_t        ri;          /* runs: the current run */
    const void*     data;        /* values: element 0 is the segment's row 0 */
    uint8_t         attrs;
} agg_firstn_kr_t;

static inline int64_t agg_firstn_run_val(const ray_enc_vview_t* vv, uint32_t r) {
    return ray_enc_logical(agg_vv_lane(&vv->run_vals, vv->k.T, r), vv->k);
}

/* The other key's reader at segment row 0, made on first use. */
static ray_t* agg_firstn_kr_open(const agg_rr_t* rr, agg_curs_t* cs, uint32_t k, int64_t seg, int64_t len,
                                 ray_enc_vview_t* vv, agg_firstn_kr_t* kr) {
    const int64_t b = seg * RAY_MORSEL_ELEMS;
    ray_t* err = NULL;
    kr->vv = vv; kr->ri = 0;
    int r = agg_firstn_runview(rr, k, seg, len, vv, &err);
    if (r < 0) return err;
    kr->runs = r > 0;
    if (kr->runs) return NULL;
    if (!rr->srcs[k]) {
        kr->data = (const char*)rr->plain[k] + (size_t)b * rr->esz[k];
        kr->attrs = rr->key_attrs[k];
        return NULL;
    }
    const ray_col_range_t* v;
    if ((err = ray_col_cursor_at(&cs->cur[k], b, &v)) != NULL) return err;
    kr->data = NULL;                   /* the segment across views (a published materialization
                                        * read from an earlier row): the caller walks it */
    if (v->start > b || v->start + v->n < b + len) return NULL;
    kr->data = (const char*)v->data + (size_t)(b - v->start) * ray_sym_elem_size(v->type, v->attrs);
    kr->attrs = v->attrs;
    return NULL;
}

/* Pass 2 of segment `seg` (one vector: single-file vectors are morsel
 * aligned) driven by a key whose vector is a run view: a run whose value
 * no group holds (agg_firstn_member) is skipped whole, and only a member
 * run reads the other key, by its runs (a member sub-span probed once)
 * or by its values (each row probed).  Nothing is decoded but the other
 * key's vector, and only when a member run needs it.  A row matches when
 * its whole tuple is in the set (agg_firstn_probe), as in the plain pass;
 * matches go out in row order.  1 served, 0 no key is a run view (the
 * caller walks the segment), -1 with *err. */
/* One or two keys (the driving key and the other one), their views on
 * the task's stack (vv[AGG_CURS_LENT]). */
_Static_assert(AGG_CURS_LENT >= 2, "agg_firstn_runs keeps two run views");
static int agg_firstn_runs(agg_firstn_piece_t* t, agg_curs_t* cs, ray_enc_vview_t* vv, int64_t seg, int64_t len,
                           ray_t** err) {
    const agg_firstn_ctx_t* c = t->c;
    const agg_rr_t* rr = c->rr;
    agg_firstn_set_t* set = c->set;
    const uint32_t nk = rr->n_keys;
    uint32_t dk = 0;
    int r = 0;
    for (; dk < nk; dk++) {
        r = agg_firstn_runview(rr, dk, seg, len, &vv[0], err);
        if (r != 0) break;
    }
    if (r <= 0) return r;                  /* -1, or no key on runs */
    const uint32_t ok = nk == 2 ? 1 - dk : 0;   /* the other key */
    agg_firstn_kr_t o = { .runs = false };
    bool o_open = false;
    const ray_enc_vview_t* dv = &vv[0];
    const int64_t b = seg * RAY_MORSEL_ELEMS;
    int64_t q = t->ix ? t->j : 0;
    int64_t p = 0;
    for (uint32_t ri = 0; ri < dv->n_runs && p < len; ri++) {
        const int64_t e = dv->run_ends.h[ri];
        const int64_t dval = agg_firstn_run_val(dv, ri);
        /* the run's rows of interest: [p, e), or its selected rows */
        int64_t q1 = q;
        if (t->ix) while (q1 < t->j_end && t->ix[q1] < e) q1++;
        bool any = t->ix ? q1 > q : true;
        if (!any || !agg_firstn_member(c, dk, dval)) { p = e; q = q1; continue; }
        t->kv[dk] = dval;
        if (nk == 1) {
            if (agg_firstn_probe(set, t->kv, agg_firstn_hash(t->kv, 1), false) >= 0) {
                if (t->ix) for (int64_t z = q; z < q1; z++) { if (t->n < t->out_cap) t->out[t->n] = b + t->ix[z]; t->n++; }
                else for (int64_t z = p; z < e; z++) { if (t->n < t->out_cap) t->out[t->n] = b + z; t->n++; }
            }
            p = e; q = q1;
            continue;
        }
        if (!o_open) {
            if ((*err = agg_firstn_kr_open(rr, cs, ok, seg, len, &vv[1], &o)) != NULL) return -1;
            if (!o.runs && !o.data) return 0;   /* nothing emitted yet: the caller walks the segment */
            o_open = true;
        }
        /* the run's rows, in order: every row, or the selected ones */
        for (int64_t z = t->ix ? q : p; z < (t->ix ? q1 : e); ) {
            const int64_t row = t->ix ? t->ix[z] : z;
            int64_t span_end;          /* rows [row, span_end) share the other key's value */
            int64_t oval;
            if (o.runs) {
                while (o.vv->run_ends.h[o.ri] <= row) o.ri++;
                oval = agg_firstn_run_val(o.vv, o.ri);
                span_end = o.vv->run_ends.h[o.ri] < e ? o.vv->run_ends.h[o.ri] : e;
            } else {
                oval = agg_read_key_i64_raw(rr->key_types[ok], o.attrs, o.data, row);
                span_end = row + 1;
            }
            int64_t z1 = z + 1;        /* the indices the span covers */
            if (t->ix) while (z1 < q1 && t->ix[z1] < span_end) z1++;
            else z1 = span_end;
            if (agg_firstn_member(c, ok, oval)) {
                t->kv[ok] = oval;
                if (agg_firstn_probe(set, t->kv, agg_firstn_hash(t->kv, 2), false) >= 0)
                    for (int64_t w = z; w < z1; w++) {
                        if (t->n < t->out_cap) t->out[t->n] = b + (t->ix ? t->ix[w] : w);
                        t->n++;
                    }
            }
            z = z1;
        }
        p = e; q = q1;
    }
    if (t->ix && q < t->j_end)         /* a selected row past the segment */
        { *err = ray_error("corrupt", "first-n: selection row %lld past its segment", (long long)(b + t->ix[q])); return -1; }
    if (t->ix) t->j = t->j_end;
    return 1;
}

/* agg_firstn_task over staged keys: its segments walked through one cursor
 * set (ray_col_walk_cur), a range error noted keyed by its first row.  With
 * one or two keys, a segment driven by a key on runs is served on runs
 * (agg_firstn_runs, the views on the task's stack). */
static void agg_firstn_task_staged(agg_firstn_ctx_t* c, uint32_t t) {
    int64_t s0 = (int64_t)c->n_segs * t / c->tasks;
    int64_t s1 = (int64_t)c->n_segs * (t + 1) / c->tasks;
    const agg_rr_t* rr = c->rr;
    const uint8_t*  fl = c->sel ? ray_rowsel_flags(c->sel) : NULL;
    const uint32_t* of = c->sel ? ray_rowsel_offsets(c->sel) : NULL;
    uint64_t hb[RAY_MORSEL_ELEMS];
    ray_enc_vview_t vv[AGG_CURS_LENT];     /* agg_firstn_runs' views, never zeroed */
    agg_firstn_piece_t pc = { .c = c, .hb = hb, .kv = c->kvs + (int64_t)t * c->n_keys,
        .out = c->out ? c->out + c->counts[t] : c->local + (int64_t)t * AGG_FIRSTN_LOCAL,
        .out_cap = c->out ? INT64_MAX : AGG_FIRSTN_LOCAL, .n = 0 };
    const int64_t key = s0 * RAY_MORSEL_ELEMS;
    agg_curs_t cs; cs.cur = NULL;
    if (s0 < s1 && !ray_scan_fail_skip(rr->fail, key)) {
        if (!agg_curs_init(&cs, rr)) ray_scan_fail_note(rr->fail, key, ray_error("oom", NULL));
        for (int64_t seg = s0; cs.cur && seg < s1; seg++) {
            if (fl && fl[seg] == RAY_SEL_NONE) continue;
            int64_t b = seg * RAY_MORSEL_ELEMS;
            int64_t len = c->nrows - b < RAY_MORSEL_ELEMS ? c->nrows - b : RAY_MORSEL_ELEMS;
            bool mix = fl && fl[seg] == RAY_SEL_MIX;
            pc.seg_b = b;
            pc.ix = mix ? ray_rowsel_idx(c->sel) : NULL;
            pc.j = mix ? (int64_t)of[seg] : 0;
            pc.j_end = mix ? (int64_t)of[seg + 1] : 0;
            ray_t* err = NULL;
            int ru = rr->n_keys <= 2 ? agg_firstn_runs(&pc, &cs, vv, seg, len, &err) : 0;
            if (ru < 0) { ray_scan_fail_note(rr->fail, key, err); break; }
            if (ru > 0) continue;
            err = ray_col_walk_cur(rr->srcs, rr->plain, rr->n_src, cs.cur, b, b + len, &pc, 0, agg_firstn_piece);
            if (err) { ray_scan_fail_note(rr->fail, key, err); break; }
        }
        agg_curs_free(&cs, rr);
    }
    if (!c->out) c->counts[t] = pc.n;
}

static void agg_firstn_fn_staged(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    for (int64_t t = start; t < end; t++) agg_firstn_task_staged((agg_firstn_ctx_t*)raw, (uint32_t)t);
}

static bool agg_firstn_admitted(ray_graph_t* g, ray_op_ext_t* ext, ray_t* tbl,
                                ray_t** key_cols) {
    for (uint32_t k = 0; k < ext->n_keys; k++)
        switch (key_cols[k]->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        uint16_t aop = ext->agg_ops[a];
        if (aop != OP_COUNT && aop != OP_SUM && aop != OP_AVG && aop != OP_MIN && aop != OP_MAX)
            return false;
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) return false;
        bool has_in = ext->agg_ins && ext->agg_ins[a] != RAY_OP_NONE;
        if (!has_in) { if (aop == OP_COUNT) continue; return false; }
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        if (!ie || ie->base.opcode != OP_SCAN) return false;
        ray_col_meta_t cm;
        if (!agg_col_meta(g, tbl, ie->sym, &cm)) return false;
        if (aop == OP_COUNT) continue;
        switch (cm.type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME: case RAY_TIMESTAMP: break;
            default: return false;   /* floats: the fold order shows in the value */
        }
    }
    return true;
}

/* Map the rows of a group result (key columns okc, sub_rows rows) to the
 * set's ranks: perm[rank] = row.  Rows of groups outside the set are skipped,
 * or fail the pick (-1) when `strict` (every row must be a set group).  The
 * key columns are hashed a block at a time; only bloom hits read the tuple
 * and probe.  Returns the number of set groups found, -1 on a mismatch.  Kept
 * out of line: the pre-pass's serial probe loop stays small. */
static __attribute__((noinline)) int64_t agg_firstn_pick(
        agg_firstn_set_t* set, ray_t** okc, uint32_t n_keys, int64_t sub_rows,
        uint32_t bbits, bool strict, int64_t* perm, int64_t* kv, bool* identity) {
    uint64_t hb[1024];
    const void* okd[16];
    for (uint32_t k = 0; k < n_keys; k++) okd[k] = ray_data(okc[k]);
    uint32_t bshift = 64 - bbits;
    int64_t found = 0;
    for (int64_t b0 = 0; b0 < sub_rows; b0 += 1024) {
        int64_t len = sub_rows - b0 < 1024 ? sub_rows - b0 : 1024;
        for (int64_t i = 0; i < len; i++) hb[i] = AGG_FIRSTN_SEED;
        for (uint32_t k = 0; k < n_keys; k++) agg_firstn_hash_block(hb, okc[k], okd[k], b0, len);
        for (int64_t i = 0; i < len; i++) {
            int64_t r = b0 + i;
            uint64_t hv = AGG_FIRSTN_FINAL(hb[i]);
            uint64_t bit = hv >> bshift;
            int64_t rk = -1;
            if ((set->bloom[bit >> 6] >> (bit & 63)) & 1) {
                for (uint32_t k = 0; k < n_keys; k++) kv[k] = agg_read_key_i64(okc[k], okd[k], r);
                rk = agg_firstn_probe(set, kv, hv, false);
            }
            if (rk < 0) { if (strict) return -1; continue; }
            if (perm[rk] >= 0) return -1;
            perm[rk] = r;
            found++;
            if (rk != r) *identity = false;
        }
    }
    return found;
}

/* The first `group_limit` groups in first-seen order, an error, or NULL when
 * the shape or the data is not one the pre-pass serves (the caller then runs
 * the full aggregation). */
static ray_t* agg_first_n_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
                              ray_t** key_cols, agg_keyset_t* ks, ray_t* sel, const int64_t* sel_prefix,
                              int64_t n_sel, int64_t group_limit,
                              bool allow_heavy, bool* heavy_out) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    ray_pool_t* pool = ray_pool_get();
    int64_t eff_n = sel ? n_sel : nrows;
    /* A segment-streamed table (an unordered take over generation-2 parted
     * keys, group_seg_stream_ok) runs the passes over its parted sources;
     * every decline then runs the whole group over its flat copy, the only
     * full run its unordered take has. */
    const bool seg = group_limit > 0 && agg_seg_parted(g, tbl);
    /* Only where the full run would take the parallel first-seen routes. */
    if (!ray_agg_first_n || firstn_nested || !pool || eff_n < RAY_PARALLEL_THRESHOLD ||
        group_limit <= 0 || group_limit > AGG_FIRSTN_MAX || !ext ||
        (sel && ray_rowsel_meta(sel)->nrows != nrows) || !agg_firstn_admitted(g, ext, tbl, key_cols))
        return seg ? agg_seg_flat_run(g, op, tbl, nrows, sel, sel_prefix, n_sel, group_limit, NULL) : NULL;
    /* Staged keys (generation 2) are read through their sources at rows, by
     * the staged instances of the passes; nothing is unstaged.  A declined
     * pre-pass leaves the full run on the staged keys. */
    const agg_rr_t* krr = ks->rr.staged ? &ks->rr : NULL;

    uint32_t n_keys = ext->n_keys;
    uint32_t bits = 4;
    while (((int64_t)1 << bits) < group_limit * 4) bits++;
    uint32_t bbits = bits + 6 > 16 ? bits + 6 : 16;
    int64_t cap = (int64_t)1 << bits;
    ray_t* res = NULL;
    ray_t* kv_hdr = NULL;
    /* one carve: the probe tuple, then the key data pointers */
    int64_t* kv = (int64_t*)scratch_alloc(&kv_hdr, (size_t)n_keys * 2 * sizeof(int64_t));
    agg_firstn_set_t set = { .n_keys = n_keys, .bits = bits, .bbits = bbits, .n = 0 };
    set.keys = ray_alloc_raw((size_t)cap * n_keys * sizeof(int64_t));
    set.rank = ray_alloc_raw((size_t)cap * sizeof(int32_t));
    set.bloom = ray_calloc_raw(((size_t)1 << bbits) / 8);
    int64_t* counts = NULL;
    int64_t* local = NULL;
    int64_t* kvs = NULL;
    int64_t* idx = NULL;
    int64_t* perm = NULL;
    ray_t* compact = NULL;
    ray_t* sub = NULL;
    int64_t* mem_v = NULL;
    uint8_t* mem_used = NULL;
    if (!kv || !set.keys || !set.rank || !set.bloom) { res = ray_error("oom", NULL); goto done; }
    memset(set.rank, 0xFF, (size_t)cap * sizeof(int32_t));
    const void** key_data = (const void**)(kv + n_keys);
    for (uint32_t k = 0; k < n_keys; k++) key_data[k] = ray_data(key_cols[k]);
    uint32_t n_segs = (uint32_t)((nrows + RAY_MORSEL_ELEMS - 1) / RAY_MORSEL_ELEMS);

    /* 1. the first N distinct tuples, serially from the first selected row */
    if (krr) {
        ray_t* e1 = agg_firstn_seen_staged(krr, &set, sel, nrows, n_segs, group_limit, kv);
        if (e1) { res = e1; goto done; }
        if (set.n < group_limit) goto done;   /* few groups or long runs: full run */
    } else {
        const uint8_t*  fl = sel ? ray_rowsel_flags(sel) : NULL;
        const uint32_t* of = sel ? ray_rowsel_offsets(sel) : NULL;
        const uint16_t* ix = sel ? ray_rowsel_idx(sel) : NULL;
        int64_t budget = group_limit * 64 > AGG_FIRSTN_PROBE_ROWS
                       ? group_limit * 64 : AGG_FIRSTN_PROBE_ROWS;
        int64_t scanned = 0;
        for (int64_t seg = 0; seg < n_segs && set.n < group_limit && scanned < budget; seg++) {
            if (fl && fl[seg] == RAY_SEL_NONE) continue;
            int64_t b = seg * RAY_MORSEL_ELEMS;
            int64_t len = nrows - b < RAY_MORSEL_ELEMS ? nrows - b : RAY_MORSEL_ELEMS;
            bool mix = fl && fl[seg] == RAY_SEL_MIX;
            int64_t j_end = mix ? (int64_t)of[seg + 1] : len;
            for (int64_t j = mix ? (int64_t)of[seg] : 0;
                 j < j_end && set.n < group_limit && scanned < budget; j++, scanned++) {
                int64_t row = b + (mix ? (int64_t)ix[j] : j);
                for (uint32_t k = 0; k < n_keys; k++)
                    kv[k] = agg_read_key_i64(key_cols[k], key_data[k], row);
                agg_firstn_probe(&set, kv, agg_firstn_hash(kv, n_keys), true);
            }
        }
        if (set.n < group_limit) goto done;   /* few groups or long runs: full run */
    }
    for (int64_t i = 0; i < cap; i++) {
        if (set.rank[i] < 0) continue;
        uint64_t bit = agg_firstn_hash(set.keys + i * n_keys, n_keys) >> (64 - bbits);
        set.bloom[bit >> 6] |= (uint64_t)1 << (bit & 63);
    }
    if (krr) {
        /* the staged pass's per-key member sets (agg_firstn_member): at
         * most N values each, in as many slots as the set has */
        mem_v = ray_alloc_raw((size_t)cap * n_keys * sizeof(int64_t));
        mem_used = ray_calloc_raw((size_t)cap * n_keys);
        if (!mem_v || !mem_used) { res = ray_error("oom", NULL); goto done; }
        for (int64_t i = 0; i < cap; i++) {
            if (set.rank[i] < 0) continue;
            for (uint32_t k = 0; k < n_keys; k++) {
                int64_t x = set.keys[i * n_keys + k];
                int64_t* v = mem_v + (size_t)k * cap;
                uint8_t* u = mem_used + (size_t)k * cap;
                uint32_t h = agg_firstn_mslot(x, bits);
                while (u[h] && v[h] != x) h = (h + 1) & (uint32_t)(cap - 1);
                u[h] = 1; v[h] = x;
            }
        }
    }

    /* the share of rows the N groups hold, from evenly spaced source rows */
    if (krr) {
        int64_t samples = nrows < AGG_FIRSTN_SAMPLES ? nrows : AGG_FIRSTN_SAMPLES, hits = 0;
        ray_t* e1 = agg_firstn_share_staged(krr, &set, nrows, samples, pool,
                                            ray_pool_par_dispatch_ok(pool, eff_n, RAY_PARALLEL_THRESHOLD), &hits);
        if (e1) { res = e1; goto done; }
        if (hits * AGG_FIRSTN_SHARE > samples) goto heavy;
    } else {
        int64_t samples = nrows < AGG_FIRSTN_SAMPLES ? nrows : AGG_FIRSTN_SAMPLES;
        int64_t step = nrows / samples, hits = 0;
        for (uint32_t k = 0; k < n_keys; k++)
            if (ray_col_cold(key_cols[k]))
                for (int64_t i = 0; i < samples; i++) ray_col_want_row(key_cols[k], i * step);
        for (int64_t i = 0; i < samples; i++) {
            for (uint32_t k = 0; k < n_keys; k++)
                kv[k] = agg_read_key_i64(key_cols[k], key_data[k], i * step);
            if (agg_firstn_probe(&set, kv, agg_firstn_hash(kv, n_keys), false) >= 0) hits++;
        }
        if (hits * AGG_FIRSTN_SHARE > samples) goto heavy;
    }

    /* 2. the selected rows of those groups, in row order: each task keeps its
     * first AGG_FIRSTN_LOCAL matches; a second pass writes them all only when
     * some task had more. */
    uint32_t tasks = 1;
    if (ray_pool_par_dispatch_ok(pool, eff_n, RAY_PARALLEL_THRESHOLD)) {
        uint32_t want = ray_pool_total_workers(pool) * 4;
        if (want > RAY_POOL_INIT_TASKS) want = RAY_POOL_INIT_TASKS;
        tasks = want < n_segs ? want : n_segs;
        if (tasks < 1) tasks = 1;
    }
    counts = ray_alloc_raw((size_t)tasks * sizeof(int64_t));
    local = ray_alloc_raw((size_t)tasks * AGG_FIRSTN_LOCAL * sizeof(int64_t));
    kvs = ray_alloc_raw((size_t)tasks * n_keys * sizeof(int64_t));
    if (!counts || !local || !kvs) { res = ray_error("oom", NULL); goto done; }
    agg_firstn_ctx_t c = { .set = &set, .key_cols = key_cols, .key_data = key_data,
        .n_keys = n_keys, .nrows = nrows, .sel = sel, .n_segs = n_segs,
        .tasks = tasks, .counts = counts, .local = local, .out = NULL, .kvs = kvs, .rr = krr,
        .mem_v = mem_v, .mem_used = mem_used, .mem_bits = bits };
    ray_pool_fn pass2 = krr ? agg_firstn_fn_staged : agg_firstn_fn;
    if (krr) ray_scan_fail_init(krr->fail);
    if (tasks > 1) ray_pool_dispatch_n(pool, pass2, &c, tasks);
    else if (krr) agg_firstn_fn_staged(&c, 0, 0, 1);
    else agg_firstn_fn(&c, 0, 0, 1);
    if (krr && (res = ray_scan_fail_finish(krr->fail, tasks > 1 ? pool : NULL)) != NULL) goto done;
    if (agg_cancelled()) { res = ray_error("cancel", NULL); goto done; }
    int64_t m = 0;
    bool spilled = false;
    for (uint32_t t = 0; t < tasks; t++) {
        if (counts[t] > AGG_FIRSTN_LOCAL) spilled = true;
        m += counts[t];
    }
    if (m * (AGG_FIRSTN_SHARE / 2) > eff_n) goto heavy;  /* the sample missed a heavy group */
    idx = ray_alloc_raw((size_t)(m > 0 ? m : 1) * sizeof(int64_t));
    if (!idx) { res = ray_error("oom", NULL); goto done; }
    if (spilled) {
        int64_t off = 0;
        for (uint32_t t = 0; t < tasks; t++) { int64_t ct = counts[t]; counts[t] = off; off += ct; }
        c.out = idx;
        if (krr) ray_scan_fail_init(krr->fail);
        if (tasks > 1) ray_pool_dispatch_n(pool, pass2, &c, tasks);
        else if (krr) agg_firstn_fn_staged(&c, 0, 0, 1);
        else agg_firstn_fn(&c, 0, 0, 1);
        if (krr && (res = ray_scan_fail_finish(krr->fail, tasks > 1 ? pool : NULL)) != NULL) goto done;
        if (agg_cancelled()) { res = ray_error("cancel", NULL); goto done; }
    } else {
        int64_t off = 0;
        for (uint32_t t = 0; t < tasks; t++) {
            memcpy(idx + off, local + (int64_t)t * AGG_FIRSTN_LOCAL, (size_t)counts[t] * sizeof(int64_t));
            off += counts[t];
        }
    }

    compact = agg_build_compact(g, op, tbl, idx, m);
    if (!compact || RAY_IS_ERR(compact)) {
        res = compact ? compact : ray_error("oom", NULL); compact = NULL; goto done;
    }
    /* The same bounded emit as the full run: every route keeps first-seen
     * order under a limit, and the limit is the whole group count here.  The
     * nested run must not re-enter the pre-pass. */
    firstn_nested = true;
    sub = exec_group_v2_run(g, op, compact, m, NULL, NULL, 0, set.n, NULL);
    firstn_nested = false;
    goto pick;

heavy:
    /* The N groups hold a large share of the rows: aggregating only them
     * costs about what aggregating everything does.  The bounded full run
     * is the answer when it stays on a dense route (the task-local slabs
     * select the N smallest first rows); the caller asks again with
     * allow_heavy only when it would leave them for radix.  Then run the
     * unbounded group (dense routes allowed) and pick the N groups of the
     * set from its result. */
    if (!allow_heavy) { if (heavy_out) *heavy_out = true; goto done; }
    firstn_nested = true;
    sub = exec_group_v2_run(g, op, tbl, nrows, sel, sel_prefix, n_sel, 0, NULL);
    firstn_nested = false;

pick:
    if (!sub || RAY_IS_ERR(sub)) { res = sub ? sub : ray_error("oom", NULL); sub = NULL; goto done; }

    /* 3. first-seen order: the rank of every emitted group in the set.  The
     * bounded emit already yields it; any other order is put right here;
     * the unbounded run's other groups are dropped. */
    int64_t sub_rows = sub->type == RAY_TABLE ? ray_table_nrows(sub) : -1;
    if (sub->type != RAY_TABLE || sub_rows < set.n || (compact && sub_rows != set.n) ||
        ray_table_ncols(sub) < (int64_t)n_keys)
        goto done;                             /* unexpected shape: full run */
    perm = ray_alloc_raw((size_t)set.n * sizeof(int64_t));
    if (!perm) { res = ray_error("oom", NULL); goto done; }
    for (int64_t r = 0; r < set.n; r++) perm[r] = -1;
    bool identity = sub_rows == set.n;
    ray_t* okc[16];
    if (n_keys > 16) goto done;
    for (uint32_t k = 0; k < n_keys; k++) {
        okc[k] = ray_table_col_idx(sub, k);
        if (okc[k] && RAY_IS_ERR(okc[k])) { res = okc[k]; goto done; }
        if (!okc[k] || okc[k]->type != key_cols[k]->type) goto done;
    }
    int64_t found = agg_firstn_pick(&set, okc, n_keys, sub_rows, bbits, compact != NULL,
                                    perm, kv, &identity);
    if (found != set.n) goto done;
    if (identity) { res = sub; sub = NULL; }
    else {
        ray_t* pv = ray_vec_new(RAY_I64, set.n);
        if (!pv || RAY_IS_ERR(pv)) { res = pv ? pv : ray_error("oom", NULL); goto done; }
        memcpy(ray_data(pv), perm, (size_t)set.n * sizeof(int64_t));
        pv->len = set.n;
        res = ray_at_fn(sub, pv);
        ray_release(pv);
        if (!res) res = ray_error("oom", NULL);
    }
    if (!RAY_IS_ERR(res)) {
        route_stats.first_n++;
        ray_profile_tick("group: first-N keys");
    }

done:
    if (sub) { ray_graph_unbind_table(g, sub); ray_release(sub); }
    if (compact) { ray_graph_unbind_table(g, compact); ray_release(compact); }
    ray_free_raw(perm); ray_free_raw(idx); ray_free_raw(local); ray_free_raw(kvs); ray_free_raw(counts);
    ray_free_raw(set.keys); ray_free_raw(set.rank); ray_free_raw(set.bloom);
    ray_free_raw(mem_v); ray_free_raw(mem_used);
    if (kv_hdr) scratch_free(kv_hdr);
    if (!res && seg) return agg_seg_flat_run(g, op, tbl, nrows, sel, sel_prefix, n_sel, group_limit, NULL);
    return res;
}

/* Bind the group keys (R-BIND) and run the strategies over them.  A key
 * that is a partition key or a slice keeps ray_table_col; a bound key that
 * is not ready (generation 2, not materialized) is staged: its column is a
 * stand-in carrying the type, SYM width, domain and nullability, and the
 * strategies read it by range. */
static ray_t* exec_group_v2_run_keys(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit,
                                const ray_group_emit_filter_t* efp, ray_op_ext_t* ext,
                                ray_t** key_cols, int64_t* key_syms, agg_keyset_t* ks);

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
    route_stats.radix_waves = 0;
    route_stats.radix_compacted = 0;
    route_stats.radix_spill_bytes = 0;
    route_stats.radix_spill_buf = 0;
    ray_op_ext_t* ext = find_ext(g, op->id);
    /* efp was already gated on this node by exec_group_v2_run; the compact
     * fallback recursion re-enters through that wrapper too. */

    /* Exact-size carve for the per-key columns, syms, sources and read
     * tables: unbounded key count, freed (with the stand-ins) when the
     * strategies return. */
    uint32_t nk = ext->n_keys;
    size_t nkc = nk ? nk : 1;
    ray_t* kc_hdr;
    ray_t** key_cols = (ray_t**)scratch_calloc(&kc_hdr, nkc * (5 * sizeof(void*) + 3));
    if (!key_cols) return ray_error("oom", NULL);
    int64_t* key_syms = (int64_t*)(key_cols + nkc);
    ray_t** standins = (ray_t**)(key_syms + nkc);
    agg_keyset_t ks;
    ks.srcs  = (ray_col_src_t**)(standins + nkc);
    ks.plain = (const void**)(ks.srcs + nkc);
    ks.esz   = (uint8_t*)(ks.plain + nkc);
    ks.types = (int8_t*)(ks.esz + nkc);
    ks.attrs = (uint8_t*)(ks.types + nkc);
    ray_t* result = NULL;
    bool staged = false;
    for (uint32_t k = 0; k < nk; k++) {
        ray_op_ext_t* kext = find_ext(g, ext->keys[k]);
        key_syms[k] = kext->sym;
        ray_col_src_t* src = NULL;
        if (!ray_col_bind_declines(tbl, kext->sym)) {
            ray_t* berr = NULL;
            src = ray_graph_bind_col(g, tbl, kext->sym, &berr);   /* no decode */
            if (berr) { result = berr; goto out; }
        }
        ray_t* ready = src ? ray_col_src_ready(src) : NULL;      /* never decodes */
        if (ready && !RAY_IS_PARTED(ready->type)) key_cols[k] = ready;
        else if (src) {
            if (!(standins[k] = agg_src_standin(src))) { result = ray_error("oom", NULL); goto out; }
            key_cols[k] = standins[k];
            ks.srcs[k] = src;
            staged = true;
        } else {
            key_cols[k] = ray_table_col(tbl, kext->sym);   /* today's column */
            if (key_cols[k] && RAY_IS_ERR(key_cols[k])) { result = key_cols[k]; key_cols[k] = NULL; goto out; }
        }
        ks.types[k] = key_cols[k]->type;
        ks.attrs[k] = key_cols[k]->attrs;
        ks.esz[k] = ray_sym_elem_size(key_cols[k]->type, key_cols[k]->attrs);
        ks.plain[k] = ks.srcs[k] ? NULL : ray_data(key_cols[k]);
        if (key_cols[k]->type != RAY_SYM && ray_vec_may_have_nulls(key_cols[k]))
            route_stats.nullable_key = true;
    }
    ray_scan_fail_init(&ks.fail);
    ks.rr = (agg_rr_t){ .srcs = ks.srcs, .plain = ks.plain, .esz = ks.esz,
        .key_types = ks.types, .key_attrs = ks.attrs,
        .n_src = nk, .n_keys = nk, .n_aggs = 0, .fail = &ks.fail, .staged = staged };
    result = exec_group_v2_run_keys(g, op, tbl, nrows, sel, sel_prefix, n_sel, group_limit, efp,
                                    ext, key_cols, key_syms, &ks);
out:
    for (uint32_t k = 0; k < nk; k++) if (standins[k]) ray_release(standins[k]);
    scratch_free(kc_hdr);
    return result;
}

static ray_t* exec_group_v2_run_keys(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit,
                                const ray_group_emit_filter_t* efp, ray_op_ext_t* ext,
                                ray_t** key_cols, int64_t* key_syms, agg_keyset_t* ks) {
    bool indexed = false;
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        int8_t t = key_cols[k]->type;
        if (t == RAY_F32 || t == RAY_F64 || t == RAY_GUID || t == RAY_STR || t == RAY_LIST) indexed = true;
    }
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        ray_col_meta_t cm;
        if (ie && agg_col_meta(g, tbl, ie->sym, &cm) && agg_indexed_supported(ext->agg_ops[a], cm.type)) indexed = true;
    }
    if (indexed) {
        if (sel) {
            ray_t* idx = ray_rowsel_to_indices(sel);
            if (!idx || RAY_IS_ERR(idx)) { return idx ? idx : ray_error("oom", NULL); }
            ray_t* compact = agg_build_compact(g, op, tbl, ray_data(idx), n_sel);
            ray_t* result = compact && !RAY_IS_ERR(compact)
                ? exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0, group_limit, efp) : compact;
            if (compact && !RAY_IS_ERR(compact)) { ray_graph_unbind_table(g, compact); ray_release(compact); }
            ray_release(idx); return result;
        }
        /* random: indexed / serial whole-column strategy */
        if (agg_seg_parted(g, tbl))
            return agg_seg_flat_run(g, op, tbl, nrows, sel, sel_prefix, n_sel, group_limit, efp);
        agg_route_record(AGG_ROUTE_V2_INDEXED);
        ray_t* uerr = agg_keys_unstage(key_cols, ks);
        if (uerr) return uerr;
        ray_t* result = agg_indexed_run(g, op, tbl, key_cols, key_syms, nrows);
        return result;
    }

    /* Unordered take: N — find the N groups first, aggregate only them. */
    bool firstn_heavy = false;   /* the N groups hold a large share: see agg_first_n_run */
    if (group_limit > 0 && !efp) {
        ray_t* fr = agg_first_n_run(g, op, tbl, nrows, key_cols, ks, sel, sel_prefix, n_sel,
                                    group_limit, false, &firstn_heavy);
        if (fr) { return fr; }
    }

    /* Precompute AoS state layout for the admitted aggregates. */
    agg_vo_t vo;
    if (!agg_vo_init(&vo, g, ext, tbl)) { return ray_error("oom", NULL); }
    const agg_vtable_t** vts = vo.vts; size_t* off = vo.off; size_t block = vo.block;

    /* Dense plan (low-card int/SYM keys + streaming aggs → direct index, no
     * hash).  Computed up front so both the parallel and serial dispatch can use
     * it.  In sel mode the min/max prescan walks only the SELECTED rows (O(n_sel),
     * not O(nrows)) — both correct (grouped keys ARE the selected rows) and cheap
     * for high-selectivity filters, and it can pull a sparse-but-low-card selected
     * key set into the dense path that the full-table range would have rejected. */
    dense_plan_t dp;
    ray_t* plan_err = NULL;
    bool dense = sel ? agg_dense_plan_sel(key_cols, &ks->rr, ext->n_keys, n_sel, sel, sel_prefix, &dp, &plan_err)
                     : agg_dense_plan_src(key_cols, &ks->rr, ext->n_keys, true, nrows, &dp, &plan_err);
    if (plan_err) { agg_vo_free(&vo); agg_dense_plan_free(&dp); return plan_err; }

    /* Compact-fallback helper for the non-chunked shapes (hash-fallback keys,
     * and the serial path): gather selected rows into a compact table once and
     * recurse with sel=NULL so the unmodified strategy runs over it. */
    #define AGG_RUN_COMPACT_FALLBACK()                                          \
        do {                                                                    \
            agg_vo_free(&vo);   /* not needed by the compact recursion */       \
            agg_dense_plan_free(&dp);  /* the recursion binds its own keys */ \
            ray_t* idxb = ray_rowsel_to_indices(sel);                           \
            if (!idxb) return ray_error("oom", NULL);                           \
            ray_t* compact = agg_build_compact(g, op, tbl, (int64_t*)ray_data(idxb), n_sel); \
            if (!compact || RAY_IS_ERR(compact)) {                              \
                ray_release(idxb);                                              \
                return compact ? compact : ray_error("oom", NULL);             \
            }                                                                   \
            ray_t* r = exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0,   \
                                         group_limit, efp);                     \
            ray_graph_unbind_table(g, compact); ray_release(compact); ray_release(idxb); \
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
    /* A single-key plan within the row count whose per-worker slabs could
     * never fit the dense budget (a quarter of the watermark, or the scatter
     * payload when that is less) is a wide plan too: only the one shared slab
     * can run it, and the per-worker budget check below must not be the one
     * that sends it to the radix route. */
    if (dp.ok && !dp.wide && ext->n_keys == 1 && pool && eff_n >= RAY_PARALLEL_THRESHOLD) {
        double budget = (double)eff_n * 2 * (8.0 * (ext->n_keys + ext->n_aggs + 1));
        int64_t wmb = ray_heap_mem_budget();
        if (wmb > 0 && budget > (double)wmb / 4) budget = (double)wmb / 4;
        if ((double)dp.total_slots * (double)(block + sizeof(int64_t) + 1) > budget) dp.wide = true;
    }
    /* A wide plan (one key, range past the row count) runs only as the shared
     * strategy: one state slab of the range plus its occupancy bits, which
     * must fit three quarters of the room under the anonymous watermark (the
     * rest is the emit's), and whose key shows many groups in a sample
     * (agg_wide_populated).  Any other shape, or too little room, drops the
     * plan and the grouping takes the route it took before. */
    if (dp.wide) {
        bool shared = pool && eff_n >= RAY_PARALLEL_THRESHOLD && all_streaming &&
                      group_limit <= 0 && ext->n_keys == 1;
        ray_t* werr = NULL;
        bool fits = shared && agg_wide_shared_fits(vts, ext->n_aggs, block, dp.total_slots, eff_n,
                                                   key_cols[0], &ks->rr, nrows, &werr);
        if (werr) { agg_vo_free(&vo); agg_dense_plan_free(&dp); return werr; }
        if (!fits) {
            agg_dense_plan_free(&dp);
            dp.ok = false;
            dp.wide = false;
        }
    }
    dense = dense && dp.ok;
    route_stats.dense_plan_available = dense;
    if (pool && eff_n >= RAY_PARALLEL_THRESHOLD && all_streaming) {
        /* A parallel dense plan owns one slot slab per worker. Admit it only
         * when the aggregate slot count across all workers remains O(input);
         * this is data-derived and independent of cache or RAM size. */
        uint32_t dense_workers = ray_pool_total_workers(pool);
        double scatter_budget = (double)eff_n * 2 * (8.0 * (ext->n_keys + ext->n_aggs + 1));
        int64_t watermark = ray_heap_mem_budget();
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
        bool shared_plan = dp.ok && group_limit <= 0 && (dp.wide || !sel) && ext->n_keys == 1 && dp.total_slots >= 4096;
        for (uint32_t a = 0; a < ext->n_aggs && shared_plan; a++)
            shared_plan = vts[a]->update_shared != NULL &&
                          (dp.wide || !vts[a]->shared_add);   /* an atomic add in the shared slab serves wide plans only */
        ray_t* serr = NULL;
        bool shared_ok = shared_plan && (dp.wide || (slab_bytes <= dense_budget &&
                         agg_shared_sample(g, ext, tbl, key_cols, &vo, nrows, &serr)));
        if (serr) {                       /* a failed column bind */
            agg_vo_free(&vo); agg_dense_plan_free(&dp);
            return serr;
        }
        if (shared_ok) {
            route_stats.dense_worker_budget = false;
            agg_route_record(AGG_ROUTE_V2_DENSE);
            ray_t* result = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext,
                nrows, pool, &dp, dense_workers, AGG_DENSE_SHARED, sel, sel_prefix, n_sel, efp, 0);
            agg_vo_free(&vo); agg_dense_plan_free(&dp);
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
            ray_t* perr = NULL;
            if (agg_desc_init(&payload, g, ext, tbl, key_cols, &perr)) {
                record_size = agg_partition_record(&payload, ext->n_aggs, NULL, NULL);
                agg_desc_free(&payload);
            } else if (perr) {            /* a failed column bind */
                agg_vo_free(&vo); agg_dense_plan_free(&dp);
                return perr;
            }
            double bytes = 2.0 * part_slots * parts * (block + sizeof(int64_t))
                + (double)eff_n * (sizeof(uint32_t) + record_size + (sel ? sizeof(int64_t) : 0))
                + (double)(sources + 1) * parts * sizeof(uint64_t)
                + (double)(parts + split_budget) * sizeof(agg_dense_partition_task_t);
            /* Small per-worker slabs are cheaper than another full payload
             * pass. Prefer partition ownership only once replicated state
             * traffic exceeds its row traffic; large pools/ranges still use
             * bounded shared storage. */
            /* Compare the UNCAPPED replication so the choice between the two
             * strategies does not move with the machine's cache size; the
             * cache bound applies inside the task-local branch below. */
            uint32_t local_tasks = cache_tasks < dense_workers ? cache_tasks : dense_workers;
            double local_traffic = dense_workers * slab_bytes;
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
                agg_vo_free(&vo); agg_dense_plan_free(&dp);
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
            /* a staged key: strided single rows through a cursor (R-SPARSE) */
            agg_curs_t tcs; tcs.cur = NULL;
            ray_t* terr = NULL;
            if (ks->rr.staged && !agg_curs_init(&tcs, &ks->rr)) terr = ray_error("oom", NULL);
            double touched_slots = 0;
            if (!ks->rr.staged && ray_col_cold(key_cols[0]))
                for (uint32_t w = 0; w < dense_workers; w++) {
                    int64_t start = nrows / dense_workers * w;
                    int64_t end = w + 1 == dense_workers ? nrows : nrows / dense_workers * (w + 1);
                    int64_t samples = end - start < 1024 ? end - start : 1024;
                    for (int64_t i = 0; i < samples; i++)
                        ray_col_want_row(key_cols[0],
                            start + i * ((end - start - 1) / (samples > 1 ? samples - 1 : 1)));
                }
            for (uint32_t w = 0; w < dense_workers && !terr; w++) {
                int64_t start = nrows / dense_workers * w;
                int64_t end = w + 1 == dense_workers ? nrows : nrows / dense_workers * (w + 1);
                int64_t samples = end - start < 1024 ? end - start : 1024;
                int64_t lo = INT64_MAX, hi = INT64_MIN;
                for (int64_t i = 0; i < samples && !terr; i++) {
                    int64_t r = start + i * ((end - start - 1) / (samples > 1 ? samples - 1 : 1));
                    int64_t v = agg_key_at(&ks->rr, &tcs, 0, r, &terr);
                    if (dp.nullable[0] && v == dp.nulls[0]) continue;
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (lo <= hi) touched_slots += (double)((uint64_t)hi - (uint64_t)lo) + 1;
            }
            agg_curs_free(&tcs, &ks->rr);
            if (terr) { agg_vo_free(&vo); agg_dense_plan_free(&dp); return terr; }
            if (touched_slots * (block + sizeof(int64_t) + 1) > scatter_budget / 2)
                dense_par_ok = false;
        }
        /* No per-worker or partitioned dense plan fits the budget: a single
         * key still runs as the one shared slab of a wide plan, as when its
         * slab alone passes the budget, rather than scattered by radix. */
        if (dp.ok && !dp.wide && !dense_par_ok && ext->n_keys == 1 && group_limit <= 0) {
            ray_t* werr = NULL;
            bool fits = agg_wide_shared_fits(vts, ext->n_aggs, block, dp.total_slots, eff_n,
                                             key_cols[0], &ks->rr, nrows, &werr);
            if (werr) { agg_vo_free(&vo); agg_dense_plan_free(&dp); return werr; }
            if (fits) {
                dp.wide = true;
                route_stats.dense_worker_budget = false;
                ray_profile_tick("group: wide shared after the worker budget");
                agg_route_record(AGG_ROUTE_V2_DENSE);
                ray_t* result = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext,
                    nrows, pool, &dp, ray_pool_total_workers(pool), AGG_DENSE_SHARED, sel, sel_prefix, n_sel, efp, 0);
                agg_vo_free(&vo); agg_dense_plan_free(&dp);
                return result;
            }
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
            agg_vo_free(&vo); agg_dense_plan_free(&dp); return r;
        }
        if (keys_intsym && firstn_heavy && dp.ok) {
            /* A dense plan exists but the bounded emit leaves the dense
             * routes here (partition ownership and the shared slab take no
             * limit): the unbounded group keeps them, and the N groups are
             * picked from it.  Without a dense plan both runs are radix and
             * the bounded one selects the N groups itself. */
            ray_t* fr = agg_first_n_run(g, op, tbl, nrows, key_cols, ks, sel, sel_prefix, n_sel,
                                        group_limit, true, NULL);
            if (fr) { agg_vo_free(&vo); agg_dense_plan_free(&dp); return fr; }
        }
        if (keys_intsym) {
            /* Sparse ranges and excessive dense worker traffic: pre-aggregate
             * per task and scatter only the all-distinct tails (table stack);
             * the radix scatter remains available behind the knob. */
            if (agg_force_smallhash) {
                ray_t* r = exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                        key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel);
                agg_vo_free(&vo); agg_dense_plan_free(&dp); return r;
            }
            ray_t* r = ray_agg_tablestack
                ? exec_group_v2_parallel_tablestack(g, op, tbl, nrows,
                        key_cols, key_syms, &ks->rr, vts, off, block, sel, sel_prefix, n_sel,
                        group_limit, efp)
                : NULL;
            if (!r) {   /* declined by its sample gate (or disabled) */
                agg_route_record(AGG_ROUTE_V2_RADIX);
                r = exec_group_v2_parallel_radix(g, op, tbl, nrows,
                        key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel,
                        group_limit, efp);
            }
            agg_vo_free(&vo); agg_dense_plan_free(&dp); return r;
        }
        /* Hash fallback (F64 / STR keys): not a chunked strategy — compact. */
        if (sel) AGG_RUN_COMPACT_FALLBACK();
        /* random: indexed / serial whole-column strategy */
        if (agg_seg_parted(g, tbl)) {
            agg_vo_free(&vo); agg_dense_plan_free(&dp);
            return agg_seg_flat_run(g, op, tbl, nrows, sel, sel_prefix, n_sel, group_limit, efp);
        }
        agg_route_record(AGG_ROUTE_V2_INDEXED);
        ray_t* uerr = agg_keys_unstage(key_cols, ks);
        if (uerr) { agg_vo_free(&vo); agg_dense_plan_free(&dp); return uerr; }
        { ray_t* r = agg_indexed_run(g, op, tbl, key_cols, key_syms, nrows);
          agg_vo_free(&vo); agg_dense_plan_free(&dp); return r; }
    }

    /* Serial path does not consult the vts/off tables (per-agg vt is re-resolved
     * below), so release the layout carve before it runs. */
    agg_vo_free(&vo);

    /* Serial path: keep the compact fallback when a filter is active (selected
     * count is below the parallel threshold here → small; not a perf blocker). */
    if (sel) AGG_RUN_COMPACT_FALLBACK();

    /* random: indexed / serial whole-column strategy */
    if (agg_seg_parted(g, tbl)) {
        agg_dense_plan_free(&dp);
        return agg_seg_flat_run(g, op, tbl, nrows, sel, sel_prefix, n_sel, group_limit, efp);
    }
    agg_route_record(dense ? AGG_ROUTE_V2_SERIAL_DENSE : AGG_ROUTE_V2_SERIAL_HASH);
    ray_t* uerr = agg_keys_unstage(key_cols, ks);
    if (uerr) { agg_dense_plan_free(&dp); return uerr; }
    agg_groups_t groups = {0};
    int grp_rc = dense ? agg_group_keys_dense(key_cols, nrows, &dp, &groups)
                       : agg_group_keys(key_cols, ext->n_keys, nrows, &groups);
    if (grp_rc != 0) { agg_dense_plan_free(&dp); return ray_error("oom", NULL); }

    ray_t* result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) { agg_dense_plan_free(&dp); agg_groups_free(&groups); return ray_error("oom", NULL); }

    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_t* kc = ray_group_gather(key_cols[k], groups.first_row, groups.ngroups);
        if (!kc || RAY_IS_ERR(kc)) { agg_dense_plan_free(&dp); agg_groups_free(&groups); ray_release(result); return kc ? kc : ray_error("oom", NULL); }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }
    agg_dense_plan_free(&dp);   /* key_cols/key_syms done — agg loop below reads neither */

    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        ray_t* col;
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {       /* binary agg (pearson) */
            ray_op_ext_t* ye = find_ext(g, ext->agg_ins2[a]);
            ray_t* x_col = ray_table_col(tbl, ie->sym);
            if (x_col && RAY_IS_ERR(x_col)) { agg_groups_free(&groups); ray_release(result); return x_col; }
            ray_t* y_col = ray_table_col(tbl, ye->sym);
            if (y_col && RAY_IS_ERR(y_col)) { agg_groups_free(&groups); ray_release(result); return y_col; }
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], x_col->type);
            col = agg_run_one_bin(vt, x_col, y_col, groups.gids, nrows, groups.ngroups, kparam);
        } else {
            ray_t* val_col = (ext->agg_ops[a] != OP_COUNT && ie) ? ray_table_col(tbl, ie->sym) : NULL;
            if (val_col && RAY_IS_ERR(val_col)) { agg_groups_free(&groups); ray_release(result); return val_col; }
            int8_t in_type = val_col ? val_col->type : RAY_I64;
            const agg_vtable_t* vt = agg_resolve(ext->agg_ops[a], in_type);
            col = agg_run_one(vt, val_col, groups.gids, nrows, groups.ngroups, kparam);
        }
        if (!col || RAY_IS_ERR(col)) { agg_groups_free(&groups); ray_release(result); return col ? col : ray_error("oom", NULL); }
        /* A COUNT reads no input column, so its input node need not be a
         * scan (`(count (* price 2))`) and has no ext to name it by. */
        int64_t agg_name = agg_result_col_name(ie ? ie->sym : 0, ext->agg_ops[a]);
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
        if (ext->agg_ops[a] == OP_COUNT)
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
        /* A parted column only on a segment-streamed table, from its
         * first-N pre-pass over generation-2 keys (an in-place concat group
         * runs no selection, group_seg_stream_ok): gathered by range like
         * an encoded one, a plain parted wrapper from its segments. */
        ray_t* gcol = NULL;
        ray_t* enc = ray_col_gather_slot(ray_table_get_col_enc(tbl, want[i]));
        ray_col_meta_t pm;
        if (!enc && g->grp_seg_stream && ray_table_col_meta(tbl, want[i], &pm) && RAY_IS_PARTED(pm.type))
            enc = ray_table_slot(tbl, want[i]);
        if (enc) {
            /* generation 2 (or a parted column): the survivors' vectors,
             * decoded or point-read by range (idx ascending); the gather's
             * result is as fresh as gather_by_idx's */
            ray_t* err = ray_col_gather_enc(enc, NULL, true, idx, n_sel, &gcol);
            if (err) { ray_release(compact); scratch_free(want_hdr); return err; }
            compact = ray_table_add_col(compact, want[i], gcol);
            ray_release(gcol);
            if (!compact || RAY_IS_ERR(compact)) { scratch_free(want_hdr); return compact ? compact : ray_error("oom", NULL); }
            continue;
        }
        ray_t* src = ray_table_col(tbl, want[i]);
        if (src && RAY_IS_ERR(src)) { ray_release(compact); scratch_free(want_hdr); return src; }
        if (!src) { ray_release(compact); scratch_free(want_hdr); return ray_error("nyi", NULL); }
        assert(!RAY_IS_PARTED(src->type));
        gcol = gather_by_idx(src, idx, n_sel);   /* fresh, no alias */
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
    ray_group_emit_filter_t ef = ray_group_emit_filter_active();
    const ray_group_emit_filter_t* efp = ef.enabled ? &ef : NULL;
    /* exec_group_v2_run reads g's op extensions unconditionally; a NULL
     * graph is an error here, not a request for an unfiltered run. */
    if (!g) return ray_error("nyi", NULL);
    if (!g->selection)
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
    /* TABLE / DICT cells compare in atom_eq (the fallthrough of
     * agg_list_key_eq).  Hashing their names and columns only spreads the
     * buckets: the fallback hash (type and length) puts every table of one
     * length in one bucket, a linear scan per row. */
    if (value->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(value);
        uint64_t hash = ray_hash_i64(nc);
        for (int64_t c = 0; c < nc; c++) {
            hash = ray_hash_combine(hash, ray_hash_i64(ray_table_col_name(value, c)));
            ray_t* col = ray_table_col_idx(value, c);
            if (col && RAY_IS_ERR(col)) { ray_error_free(col); col = NULL; }   /* hashes as absent */
            hash = ray_hash_combine(hash, agg_list_key_hash(col, view));
        }
        return hash;
    }
    if (value->type == RAY_DICT)
        return ray_hash_combine(agg_list_key_hash(ray_dict_keys(value), view),
                                agg_list_key_hash(ray_dict_vals(value), view));
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
        ray_t* sc = ray_table_col_idx(tbl, c);
        if (sc && RAY_IS_ERR(sc)) { agg_groups_free(&groups); ray_release(result); return sc; }
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
