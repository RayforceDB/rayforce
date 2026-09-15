/* src/ops/agg_engine.c — v2 aggregation engine. */
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "ops/hash.h"     /* ray_hash_bytes — wide (STR) group-key hashing */
#include "ops/internal.h"  /* col_vec_new, col_esz */
#include "ops/rowsel.h"    /* ray_rowsel_meta, ray_rowsel_to_indices */
#include "lang/internal.h" /* sym_domain_rep */
#include "table/sym.h"    /* ray_read_sym */
#include <stdlib.h>
#include <string.h>

/* Radix output address: high 32 bits partition, low 32 bits local group. */
typedef struct { int64_t idx; } agg_radix_order_t;

bool ray_agg_engine_v2 = true;   /* knob; default on */

static _Thread_local agg_route_stats_t route_stats;
void agg_route_reset(void) { memset(&route_stats, 0, sizeof(route_stats)); }
agg_route_stats_t agg_route_stats(void) { return route_stats; }
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
/* Write a finalized scalar cell into output column slot i, marking nulls. */
static void agg_put_cell(ray_t* out, int64_t i, ray_t* cell);
/* Dense direct-index serial grouping; defined below agg_run_one. */
static int agg_group_keys_dense(ray_t** key_cols, int64_t nrows,
                                const dense_plan_t* dp, agg_groups_t* out);
/* Binary-aggregate (pearson) serial driver; defined below agg_run_one. */
ray_t* agg_run_one_bin(const agg_vtable_t* vt, ray_t* x_col, ray_t* y_col,
                       const uint32_t* gids, int64_t nrows, int64_t ngroups,
                       int64_t kparam);

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

    /* Large float/wide-key streaming queries already have a faster parallel wide-key
     * implementation. Keep it: the shared serial index builder is for shapes
     * that need row order/buffering, and measured slower on this workload. */
    if (ext->n_keys <= 8 && ray_table_nrows(tbl) >= RAY_PARALLEL_THRESHOLD) {
        bool wide = false, indexed = false;
        for (uint32_t k = 0; k < ext->n_keys; k++) {
            ray_op_ext_t* ke = find_ext(g, ext->keys[k]);
            ray_t* col = ray_table_get_col(tbl, ke->sym);
            if (col->type == RAY_STR || col->type == RAY_GUID || col->type == RAY_F32 || col->type == RAY_F64) wide = true;
            if (col->type == RAY_LIST) indexed = true;
        }
        for (uint32_t a = 0; a < ext->n_aggs; a++) {
            ray_op_ext_t* ie = find_ext(g, ext->agg_ins[a]);
            ray_t* col = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
            if (col && agg_indexed_supported(ext->agg_ops[a], col->type)) indexed = true;
        }
        if (wide && !indexed) return AGG_V2_PARALLEL_WIDE;
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

/* ── Dense grouping eligibility selector (mirrors group.c DA path) ────────
 * Decides whether the key tuple packs into a bounded direct-index slot space
 * (gid = sum_k (key_k - min_k)*strides[k]) so grouping can skip hashing.
 * Eligible iff: 1..16 keys, every key is an integer/temporal/SYM type with no
 * nulls, and the product of per-key ranges is no larger than the input row
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
static inline int64_t agg_dense_component(const dense_plan_t* dp, uint32_t k, int64_t v) {
    return dp->nullable[k] && v == dp->nulls[k]
        ? dp->ranges[k] - 1 : v - dp->mins[k];
}
static bool agg_dense_range(dense_plan_t* dp, uint32_t k, int64_t mn, int64_t mx) {
    if (mx < mn) { dp->mins[k] = 0; dp->ranges[k] = 1; return dp->nullable[k]; }
    uint64_t span = (uint64_t)mx - (uint64_t)mn;
    if (span >= (uint64_t)INT64_MAX - (uint64_t)dp->nullable[k]) return false;
    dp->mins[k] = mn;
    dp->ranges[k] = (int64_t)span + 1 + dp->nullable[k];
    return true;
}

bool agg_dense_plan(ray_t** key_cols, uint32_t n_keys,
                    const agg_vtable_t** vts, uint32_t n_aggs,
                    int64_t nrows, dense_plan_t* out) {
    (void)vts; (void)n_aggs;   /* agg kind no longer gates dense eligibility */
    out->ok = false;
    out->n_keys = n_keys;
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

        const void* data = ray_data(kc);
        int64_t mn, mx;
        if (out->nullable[k]) {
            mn = INT64_MAX; mx = INT64_MIN;
            for (int64_t r = 0; r < nrows; r++) {
                int64_t v = agg_read_key_i64(kc, data, r);
                if (v == out->nulls[k]) continue;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            if (!agg_dense_range(out, k, mn, mx)) return false;
            continue;
        }
        /* Type/width-specialized min/max — hoist agg_read_key_i64's per-row
         * switch out of the prescan (same reason as the phaseA slot loop). */
        #define DENSE_MINMAX(T)                                         \
            do { const T* p = (const T*)data; mn = mx = (int64_t)p[0];  \
                 for (int64_t r = 1; r < nrows; r++) {                  \
                     int64_t v = (int64_t)p[r];                         \
                     if (v < mn) mn = v;                                \
                     if (v > mx) mx = v;                                \
                 } } while (0)
        switch (kc->type) {
            case RAY_I64: case RAY_TIMESTAMP: DENSE_MINMAX(int64_t); break;
            case RAY_I32: case RAY_DATE: case RAY_TIME: DENSE_MINMAX(int32_t); break;
            case RAY_I16: DENSE_MINMAX(int16_t); break;
            case RAY_U8: case RAY_BOOL: DENSE_MINMAX(uint8_t); break;
            case RAY_SYM:
                switch (kc->attrs & RAY_SYM_W_MASK) {
                    case RAY_SYM_W8:  DENSE_MINMAX(uint8_t);  break;
                    case RAY_SYM_W16: DENSE_MINMAX(uint16_t); break;
                    case RAY_SYM_W32: DENSE_MINMAX(uint32_t); break;
                    default:          DENSE_MINMAX(int64_t);  break; /* W64 */
                }
                break;
            default:
                mn = mx = agg_read_key_i64(kc, data, 0);
                for (int64_t r = 1; r < nrows; r++) {
                    int64_t v = agg_read_key_i64(kc, data, r);
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
        }
        #undef DENSE_MINMAX
        if (!agg_dense_range(out, k, mn, mx)) return false;
    }

    /* Composite packing; keep dense state O(input). */
    int64_t total = 1;
    int64_t dense_limit = nrows < (int64_t)UINT32_MAX
        ? nrows : (int64_t)UINT32_MAX;
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

/* Build a result key column of src_col's type by gathering the first-row cell
 * of each group at native (type-exact) byte width.  For SYM, adopts the source
 * domain so the intern ids resolve correctly.  Caller owns the returned column. */
static ray_t* agg_gather_key_col(ray_t* src_col, const int64_t* first_row, int64_t n) {
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
        ray_t* out = ray_vec_new(RAY_STR, n);
        if (!out || RAY_IS_ERR(out)) return out;
        out->len = n;
        for (int64_t i = 0; i < n; i++) {
            size_t len = 0;
            const char* str = first_row[i] < 0 ? "" : ray_str_vec_get(src_col, first_row[i], &len);
            ray_t* next = ray_str_vec_set(out, i, str ? str : "", len);
            if (!next || RAY_IS_ERR(next)) { ray_release(out); return next; }
            out = next;
        }
        return out;
    }
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    size_t esz = col_esz(src_col);
    const char* src = (const char*)ray_data(src_col);
    char* dst = (char*)ray_data(out);
    for (int64_t gi = 0; gi < n; gi++) {
        if (first_row[gi] < 0) { ray_vec_set_null(out, gi, true); continue; }
        memcpy(dst + (size_t)gi * esz, src + (size_t)first_row[gi] * esz, esz);
    }
    if (src_col->attrs & RAY_ATTR_HAS_NULLS) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

/* ══════════════════════════════════════════════════════════════════════
 * CHUNKED SELECTION CONSUMPTION (selection-vector model)
 *
 * When a pushed WHERE filter is active, g->selection is a rowsel bitmap (see
 * src/ops/rowsel.h: per-segment NONE/ALL/MIX flags + morsel-local idx[] for MIX
 * segments).  Rather than materialize a full O(rows-passed) index array
 * (ray_rowsel_to_indices) + a full compact column (the old prologue), the
 * chunked strategies (dense serial+parallel, radix, smallhash) consume the
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
static bool agg_dense_plan_sel(ray_t** key_cols, uint32_t n_keys, int64_t n_sel,
                               ray_t* sel, const int64_t* sel_prefix,
                               dense_plan_t* out) {
    out->ok = false;
    out->n_keys = n_keys;
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

    /* One pass over the selected rows updating every key's min/max together. */
    ray_t* pre_hdr;
    void* pre = scratch_alloc(&pre_hdr, 3u * (size_t)n_keys * 8);   /* mins,maxs,key_data */
    if (!pre) return false;
    int64_t* mins = (int64_t*)pre;
    int64_t* maxs = mins + n_keys;
    const void** key_data = (const void**)(maxs + n_keys);
    for (uint32_t k = 0; k < n_keys; k++) { mins[k] = INT64_MAX; maxs[k] = INT64_MIN; }
    for (uint32_t k = 0; k < n_keys; k++) key_data[k] = ray_data(key_cols[k]);

    int64_t rows[AGG_SEL_CHUNK];
    agg_sel_cursor_t cur;
    agg_sel_cursor_init(&cur, sel, sel_prefix, 0, n_sel);
    int64_t cn;
    while ((cn = agg_sel_cursor_next(&cur, rows)) > 0) {
        for (uint32_t k = 0; k < n_keys; k++) {
            ray_t* kc = key_cols[k]; const void* d = key_data[k];
            int64_t mn = mins[k], mx = maxs[k];
            for (int64_t i = 0; i < cn; i++) {
                int64_t v = agg_read_key_i64(kc, d, rows[i]);
                if (out->nullable[k] && v == out->nulls[k]) continue;
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            mins[k] = mn; maxs[k] = mx;
        }
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

/* ══════════════════════════════════════════════════════════════════════
 * Parallel DENSE group-by (low-card int/SYM keys; streaming OR buffered aggs).
 * Per-worker flat slabs of `total_slots` AoS group states (slot == gid via the
 * mixed-radix packing) + a per-worker first_row[slot] (INT64_MAX = untouched).
 * Phase A: each worker packs its chunk rows into slots and update_batch's into
 * its own slab.  Phase B: merge per-worker slabs into a global slab.  Phase C:
 * collect occupied slots in slot order and emit (output order unspecified).
 * No hashing.  Buffered aggs (median/top-k) malloc a per-group buffer in their
 * state; every init'd slot of every slab carries the vt->destroy lifecycle:
 * worker buffers are freed once after being merged into the global slab, and
 * the global slab's buffers are freed after finalize — exactly-once, mirroring
 * agg_table_destroy on the hash path. */

/* Per-worker dense slab. */
typedef struct {
    char*    states;     /* [total_slots * block] AoS, every slot init'd */
    int64_t* first_row;  /* [total_slots] min row idx touching slot, INT64_MAX = none */
    int      oom;
    bool     ready;
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

/* Initialize every slot's agg states in a freshly-allocated slab (min/max need
 * INT64_MAX/MIN seeds, NOT calloc-zero), and first_row to INT64_MAX. */
static int agg_dense_local_init(agg_dense_local_t* loc, int64_t total_slots,
                                const agg_vtable_t** vts, const size_t* off,
                                size_t block, uint32_t n_aggs) {
    loc->oom = 0;
    loc->states    = ray_alloc_raw((size_t)total_slots * block);
    loc->first_row = ray_alloc_raw((size_t)total_slots * sizeof(int64_t));
    if (!loc->states || !loc->first_row) { loc->oom = 1; return -1; }
    for (int64_t s = 0; s < total_slots; s++) {
        loc->first_row[s] = INT64_MAX;
        for (uint32_t a = 0; a < n_aggs; a++)
            vts[a]->init(loc->states + (size_t)s * block + off[a]);
    }
    loc->ready = true;
    return 0;
}

static void agg_dense_local_destroy(agg_dense_local_t* loc) {
    ray_free_raw(loc->states); ray_free_raw(loc->first_row);
    loc->states = NULL; loc->first_row = NULL;
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
static void agg_dense_init_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_dense_merge_ctx_t* c = raw;
    for (int64_t w = start; w < end; w++)
        agg_dense_local_init(&c->locals[w], c->slots, c->vts, c->off, c->block, c->n_aggs);
}
static void agg_dense_merge_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid; agg_dense_merge_ctx_t* c = raw;
    for (int64_t s = start; s < end; s++) {
        c->first[s] = INT64_MAX;
        for (uint32_t a = 0; a < c->n_aggs; a++) c->vts[a]->init(c->states + (size_t)s * c->block + c->off[a]);
        for (uint32_t w = 0; w < c->nw; w++) {
            agg_dense_local_t* loc = &c->locals[w];
            if (loc->first_row[s] == INT64_MAX) continue;
            if (loc->first_row[s] < c->first[s]) c->first[s] = loc->first_row[s];
            for (uint32_t a = 0; a < c->n_aggs; a++)
                c->vts[a]->merge(c->states + (size_t)s * c->block + c->off[a],
                                loc->states + (size_t)s * c->block + c->off[a], NULL);
        }
    }
}
static inline bool agg_put_cell_value(ray_t* out, int64_t i, ray_t* cell);
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
    size_t esz = col_esz(c->out);
    void* dst = ray_data(c->out);
    for (int64_t i = start; i < end; i++) {
        const void* st = c->states + (size_t)c->slots[i] * c->block + c->off;
        if (c->vt->finalize_value) any |= c->vt->finalize_value(st, (char*)dst + (size_t)i * esz);
        else {
            ray_t* cell = c->vt->finalize(st, NULL, c->param);
            any |= agg_put_cell_value(c->out, i, cell); ray_release(cell);
        }
    }
    if (any) atomic_store_explicit(&c->any_null, true, memory_order_relaxed);
}

/* Compute the dense slot for row r via mixed-radix packing. */
static inline int64_t agg_dense_slot(const agg_dense_ctx_t* c, int64_t r) {
    int64_t slot = 0;
    for (uint32_t k = 0; k < c->n_keys; k++)
        slot += agg_dense_component(c->dp, k, agg_read_key_i64(c->key_cols[k], c->key_data[k], r)) * c->dp->strides[k];
    return slot;
}

/* Phase A: per-worker dense accumulate over chunk [start,end). */
static void agg_dense_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_dense_ctx_t* c = (agg_dense_ctx_t*)vctx;
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
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0) {
            for (int64_t i = 0; i < cn; i++) {
                int64_t r = rows[i];
                int64_t slot = agg_dense_slot(c, r);   /* provably in [0,total_slots) */
                gid[i] = (uint32_t)slot;
                if (r < loc->first_row[slot]) loc->first_row[slot] = r;
            }
            if (agg_sel_accum_chunk(&c->vd, &sc, loc->states, gid, rows, cn) != 0) {
                loc->oom = 1; agg_sel_scratch_free(&sc); return;
            }
        }
        agg_sel_scratch_free(&sc);
        return;
    }

    uint32_t* cgid = ray_alloc_raw((size_t)n * sizeof(uint32_t));
    if (!cgid) { loc->oom = 1; return; }

    /* Hoist the per-row key-type dispatch out of the hot loop.  agg_dense_slot
     * calls agg_read_key_i64 — a switch(col->type) — on every row, and for SYM
     * keys ray_read_sym adds a second per-row switch on the code width.  Both
     * are loop-invariant.  For the common single-key case, branch ONCE on
     * (type, width) and run a tight typed load loop the compiler can vectorize;
     * this is the dominant cost of a low-card group-by (a 7-group count was
     * ~70% here).  Multi-key keeps the generic composite path. */
    if (c->n_keys == 1 && !c->dp->nullable[0]) {
        const void* kd = c->key_data[0];
        int64_t  kmin = c->dp->mins[0];
        int64_t  kstride = c->dp->strides[0];
        int64_t* fr = loc->first_row;
        uint32_t* cg = cgid;
        #define DENSE_SLOT1(LD)                                            \
            for (int64_t r = start; r < end; r++) {                        \
                int64_t slot = ((int64_t)(LD) - kmin) * kstride;           \
                cg[r - start] = (uint32_t)slot;                            \
                if (r < fr[slot]) fr[slot] = r;                            \
            }
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
                    int64_t slot = agg_dense_slot(c, r);
                    cg[r - start] = (uint32_t)slot;
                    if (r < fr[slot]) fr[slot] = r;
                }
        }
        #undef DENSE_SLOT1
    } else {
        for (int64_t r = start; r < end; r++) {
            int64_t slot = agg_dense_slot(c, r);   /* provably in [0,total_slots) */
            cgid[r - start] = (uint32_t)slot;
            if (r < loc->first_row[slot]) loc->first_row[slot] = r;
        }
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
    ray_free_raw(cgid);
}

/* One slab per logical task, independent of the physical worker executing it.
 * A large pool can run a memory-bounded number of dense tasks safely. */
static void agg_dense_task_fn(void* raw, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    agg_dense_ctx_t* c = raw;
    int64_t chunk = c->task_rows / c->n_tasks;
    for (int64_t task = start; task < end; task++)
        agg_dense_phaseA_fn(c, (uint32_t)task, chunk * task,
            task + 1 == c->n_tasks ? c->task_rows : chunk * (task + 1));
}

/* Parallel dense path.  Precondition: dp->ok, all aggs ACC_STREAMING, per-worker
 * budget already gated by the caller. */
static ray_t* exec_group_v2_parallel_dense(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl,
        ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext, int64_t nrows,
        ray_pool_t* pool, const dense_plan_t* dp, uint32_t nw,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel) {
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
    const int64_t* agg_syms = d.agg_syms;

    agg_dense_local_t* locals = ray_calloc_raw((size_t)((size_t)nw) * (sizeof(agg_dense_local_t)));
    if (!locals) { agg_vo_free(&vo); agg_desc_free(&d); return ray_error(agg_cancelled() ? "cancel" : "oom", NULL); }
    agg_dense_merge_ctx_t merge_ctx = { .locals = locals, .slots = total_slots,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs, .nw = nw };
    ray_pool_dispatch_n(pool, agg_dense_init_fn, &merge_ctx, nw);
    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++) if (!locals[w].ready) alloc_oom = 1;
    if (alloc_oom) {
        /* Only the fully-init'd slabs carry valid (destroyable) buffered state;
         * the slab that failed init never ran its per-slot init → its bytes are
         * uninitialized and must NOT be destroy'd (would free a garbage ptr). */
        for (uint32_t w = 0; w < nw; w++)
            if (locals[w].ready) agg_dense_slab_destroy_states(locals[w].states, total_slots, vts, off, block, n_aggs);
        for (uint32_t w = 0; w < nw; w++) agg_dense_local_destroy(&locals[w]);
        ray_free_raw(locals);
        agg_vo_free(&vo); agg_desc_free(&d);
        return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }

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

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom || agg_cancelled()) {
            for (uint32_t i = 0; i < nw; i++)
                agg_dense_slab_destroy_states(locals[i].states, total_slots, vts, off, block, n_aggs);
            for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
            ray_free_raw(locals);
            agg_vo_free(&vo); agg_desc_free(&d);
            return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
        }

    /* ── Phase B: merge per-worker slabs into a global slab (serial) ── */
    char*    gstates  = ray_alloc_raw((size_t)total_slots * block);
    int64_t* gfirst   = ray_alloc_raw((size_t)total_slots * sizeof(int64_t));
    if (!gstates || !gfirst) {
        ray_free_raw(gstates); ray_free_raw(gfirst);
        for (uint32_t i = 0; i < nw; i++)
            agg_dense_slab_destroy_states(locals[i].states, total_slots, vts, off, block, n_aggs);
        for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
        ray_free_raw(locals);
        agg_vo_free(&vo); agg_desc_free(&d);
        return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    merge_ctx.states = gstates; merge_ctx.first = gfirst;
    ray_pool_dispatch(pool, agg_dense_merge_fn, &merge_ctx, total_slots);
    if (agg_cancelled()) {
        for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
        ray_free_raw(locals); ray_free_raw(gstates); ray_free_raw(gfirst);
        agg_vo_free(&vo); agg_desc_free(&d); return ray_error("cancel", NULL);
    }
    /* Worker buffered state has been merged into the global slab → its per-group
     * buffers are now redundant.  Destroy (free) them exactly once before
     * releasing the worker slabs.  No-op for all-streaming. */
    for (uint32_t i = 0; i < nw; i++)
        agg_dense_slab_destroy_states(locals[i].states, total_slots, vts, off, block, n_aggs);
    for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
    ray_free_raw(locals);

    /* ── Phase C: collect occupied slots in slot order, emit. ──
     * Group-by output order is UNSPECIFIED, so we emit in dense-slot order
     * (the natural build order) rather than sorting by first_row.  gfirst[s] is
     * still the gather index (any member row) for the key columns. */
    int64_t ng = 0;
    for (int64_t s = 0; s < total_slots; s++) if (gfirst[s] != INT64_MAX) ng++;

    int64_t* occupied_slot     = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    int64_t* first_row_ordered = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(int64_t));
    if (!occupied_slot || !first_row_ordered) {
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered);
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(gstates); ray_free_raw(gfirst);
        agg_vo_free(&vo); agg_desc_free(&d);
        return ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }
    { int64_t i = 0;
      for (int64_t s = 0; s < total_slots; s++)
          if (gfirst[s] != INT64_MAX) { first_row_ordered[i] = gfirst[s]; occupied_slot[i] = s; i++; }
    }

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
        ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
        agg_vo_free(&vo); agg_desc_free(&d);
        return result ? result : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
    }

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
            agg_vo_free(&vo); agg_desc_free(&d);
            ray_release(result); return kc ? kc : ray_error(agg_cancelled() ? "cancel" : "oom", NULL);
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
            agg_vo_free(&vo); agg_desc_free(&d);
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
            if (is_list) {
                out = ray_list_set(out, i, cell);   /* retains cell */
                ray_release(cell);                  /* drop our local ref */
            } else {
                agg_put_cell(out, i, cell);
                ray_release(cell);
            }
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
    agg_vo_free(&vo); agg_desc_free(&d);
    if (result && !RAY_IS_ERR(result) && agg_cancelled()) { ray_release(result); return ray_error("cancel", NULL); }
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
        int64_t group_limit);

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
        ray_t* kc = agg_gather_key_col(key_cols[k], gt.first_row, ng);
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
            ray_t* cell = vts[a]->finalize(gt.states + (size_t)i * block + off[a], NULL, kparam);
            agg_put_cell(out, i, cell);
            ray_release(cell);
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
 * so the round-trip stores byte-identical payload to what agg_gather_key_col's
 * raw memcpy of the original column produced — for SYM it routes through
 * ray_write_sym at the matching width, and the domain is adopted from src_col
 * just like agg_gather_key_col.  Caller owns the returned column.
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
 *   - agg_put_cell would OR RAY_ATTR_HAS_NULLS into the SHARED out->attrs (a
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
            if (c->pairs[i].idx != -1) c->pairs[w++].idx = c->pairs[i].idx;
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
 * to the serial agg_put_cell + ray_vec_set_null pair: a null cell stores the
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
            if (c->vts[a]->finalize_value) {
                if (c->vts[a]->finalize_value(state, (char*)ray_data(out) + (size_t)i * col_esz(out))) any_null = true;
            } else {
                ray_t* cell = c->vts[a]->finalize(state, NULL, kparam);
                if (agg_put_cell_value(out, i, cell)) any_null = true;
                ray_release(cell);
            }
        }
        if (any_null) c->saw_null[(size_t)wid * n_aggs + a] = 1;
    }
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
    /* Heap-sort the selected entries into ascending first_row order. */
    if (ok) {
        for (int64_t end = hn - 1; end > 0; end--) {
            int64_t tk = hkey[0]; hkey[0] = hkey[end]; hkey[end] = tk;
            int64_t tp = sel_pairs[0].idx;
            sel_pairs[0].idx = sel_pairs[end].idx; sel_pairs[end].idx = tp;
            int64_t i = 0;
            for (;;) {
                int64_t l = 2 * i + 1, r = l + 1, m = i;
                if (l < end && hkey[l] > hkey[m]) m = l;
                if (r < end && hkey[r] > hkey[m]) m = r;
                if (m == i) break;
                tk = hkey[m]; hkey[m] = hkey[i]; hkey[i] = tk;
                tp = sel_pairs[m].idx;
                sel_pairs[m].idx = sel_pairs[i].idx; sel_pairs[i].idx = tp;
                i = m;
            }
        }
        ok = hn == n_emit;
    }
    scratch_free(hk_hdr);
    if (!ok) { ray_free_raw(sel_pairs); *rc = 2; return NULL; }
    return sel_pairs;
}

static ray_t* exec_group_v2_parallel_radix(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel,
        int64_t group_limit) {
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

    /* Phase 2: per-partition group+accumulate. */
    int oom = ctx.phase1_oom;
    if (!oom)
        ray_pool_dispatch_n(pool, agg_radix_group_fn, &ctx, n_parts);
    if (!oom)
        for (uint32_t p = 0; p < n_parts; p++)
            if (parts[p].oom) { oom = 1; break; }

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
    if (group_limit > 0 && ng > group_limit) {
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
    /* -1 fill as bytes: 0xFF.. == -1 for int64, and memset vectorizes —
     * this is an 80MB serial touch on a 10M-row input, worth the idiom. */
    memset(pairs, 0xFF,
           (size_t)(input_count > 0 ? input_count : 1) * sizeof(agg_radix_order_t));
    bool order_ok = true;
    int64_t ordered = 0;
    bool ord_parallel_done = false;
    if (pool && nw > 1 && input_count >= (1 << 20)) {
        const int64_t ORD_CHUNK = 1 << 17;
        int64_t n_chunks = (input_count + ORD_CHUNK - 1) / ORD_CHUNK;
        ray_t* ordcnt_hdr = NULL;
        int64_t* ord_counts = (int64_t*)scratch_alloc(&ordcnt_hdr,
            (size_t)n_chunks * sizeof(int64_t));
        if (ord_counts) {
            agg_ord_scatter_ctx_t sctx = {
                .parts = parts, .pairs = pairs,
                .input_count = input_count, .fail = 0,
            };
            ray_pool_dispatch(pool, agg_ord_scatter_fn, &sctx,
                              (int64_t)n_parts);
            if (!atomic_load_explicit(&sctx.fail, memory_order_relaxed)) {
                agg_ord_compact_ctx_t cctx = {
                    .pairs = pairs, .input_count = input_count,
                    .chunk = ORD_CHUNK, .counts = ord_counts,
                };
                ray_pool_dispatch(pool, agg_ord_count_fn, &cctx, n_chunks);
                /* Exclusive prefix (serial over a few hundred chunks). */
                int64_t run = 0;
                for (int64_t ch = 0; ch < n_chunks; ch++) {
                    int64_t n = ord_counts[ch];
                    ord_counts[ch] = run;
                    run += n;
                }
                ray_pool_dispatch(pool, agg_ord_compact_fn, &cctx, n_chunks);
                ordered = run;
                order_ok = ordered == ng;
                ord_parallel_done = true;
            } else {
                order_ok = false;
                ord_parallel_done = true;   /* bounds violation → error path */
            }
            scratch_free(ordcnt_hdr);
        }
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
     * original columns at first_row[].  Byte-identical to agg_gather_key_col
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
                ray_t* cell = vts[a]->finalize(parts[p].states + (size_t)gg * block + off[a], NULL, kparams[a]);
                if (is_list) {
                    out = ray_list_set(out, i, cell);   /* retains cell */
                    ray_release(cell);                  /* drop our local ref */
                } else {
                    agg_put_cell(out, i, cell);
                    ray_release(cell);
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
    if (rc) return ray_error("oom", NULL);
    if (agg_cancelled()) { agg_groups_free(&groups); return ray_error("cancel", NULL); }
    int64_t ng = groups.ngroups;
    int64_t* counts = ray_calloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* offsets = ray_calloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* cursor = ray_calloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* rows = ray_alloc_raw((size_t)(nrows + 1) * sizeof(int64_t));
    ray_t* result = NULL;
    if (!counts || !offsets || !cursor || !rows) { result = ray_error("oom", NULL); goto done; }
    for (int64_t r = 0; r < nrows; r++) counts[groups.gids[r]]++;
    for (int64_t i = 0; i < ng; i++) offsets[i + 1] = offsets[i] + counts[i];
    memcpy(cursor, offsets, (size_t)ng * sizeof(int64_t));
    for (int64_t r = 0; r < nrows; r++) rows[cursor[groups.gids[r]]++] = r;
    result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) goto done;
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_t* col = agg_gather_key_col(keys[k], groups.first_row, ng);
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
                for (int64_t gi = 0; gi < ng; gi++) {
                    int64_t best = -1;
                    for (int64_t j = offsets[gi]; j < offsets[gi + 1]; j++) {
                        int64_t r = rows[j];
                        if (ray_vec_is_null(src, r)) continue;
                        if (best < 0 || kind == OP_LAST) best = r;
                        if (kind == OP_FIRST) break;
                        if (kind == OP_MIN || kind == OP_MAX) {
                            const void* data = ray_data(src);
                            ray_t* x = ray_sym_domain_str(ray_sym_vec_domain(src), ray_read_sym(data, r, src->type, src->attrs));
                            ray_t* y = ray_sym_domain_str(ray_sym_vec_domain(src), ray_read_sym(data, best, src->type, src->attrs));
                            int cmp = ray_str_cmp(x, y);
                            if (kind == OP_MIN ? cmp < 0 : cmp > 0) best = r;
                        }
                    }
                    winners[gi] = best;
                }
                col = agg_gather_key_col(src, winners, ng);
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
            col = agg_run_one_bin(agg_resolve(kind, src->type), src,
                ray_table_get_col(tbl, ye->sym), groups.gids, nrows, ng, param);
        } else col = agg_run_one(agg_resolve(kind, src ? src->type : RAY_I64),
                                    kind == OP_COUNT ? NULL : src, groups.gids, nrows, ng, param);
        if (!col || RAY_IS_ERR(col)) { ray_release(result); result = col; goto done; }
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
static ray_t* exec_group_v2_run(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t nrows, ray_t* sel,
                                const int64_t* sel_prefix, int64_t n_sel,
                                int64_t group_limit) {
    agg_route_reason(AGG_V2_ADMITTED);
    route_stats.nullable_key = false;
    route_stats.dense_plan_available = false;
    route_stats.dense_worker_budget = false;
    route_stats.dense_tasks = 0;
    ray_op_ext_t* ext = find_ext(g, op->id);

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
                ? exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0, group_limit) : compact;
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
            scratch_free(kc_hdr);  /* key_cols dead: the recursion rebuilds them */ \
            ray_t* idxb = ray_rowsel_to_indices(sel);                           \
            if (!idxb) return ray_error("oom", NULL);                           \
            ray_t* compact = agg_build_compact(g, op, tbl, (int64_t*)ray_data(idxb), n_sel); \
            if (!compact || RAY_IS_ERR(compact)) {                              \
                ray_release(idxb);                                              \
                return compact ? compact : ray_error("oom", NULL);             \
            }                                                                   \
            ray_t* r = exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0,   \
                                         group_limit);                          \
            ray_release(compact); ray_release(idxb);                            \
            return r;                                                           \
        } while (0)

    /* Effective row count for the parallel-vs-serial decision: the selected
     * count when a filter is active, else the full table. */
    int64_t eff_n = sel ? n_sel : nrows;

    /* Buffered accumulators retain every contributing value per group.  Running
     * them through the parallel radix strategy overlaps those buffers with the
     * full scatter payload and can exhaust the heap on large inputs.  Keep
     * buffered shapes on the serial v2 driver; streaming shapes retain all
     * parallel strategies below. */
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
        double slab_bytes = dp.ok ? (double)dp.total_slots * (block + sizeof(int64_t)) : 0;
        if (dp.ok && slab_bytes > 0) {
            double fit = (dense_budget - (double)eff_n * sizeof(uint32_t)) / slab_bytes - 1;
            if (fit < dense_workers) dense_workers = fit >= 2 ? (uint32_t)fit : 0;
        }
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
            if (touched_slots * (block + sizeof(int64_t)) > scatter_budget / 2)
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
            route_stats.dense_tasks = dense_workers;
            agg_route_record(AGG_ROUTE_V2_DENSE);
            ray_t* r = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext, nrows, pool, &dp, dense_workers,
                                                    sel, sel_prefix, n_sel);
            agg_vo_free(&vo); scratch_free(kc_hdr); return r;
        }
        if (keys_intsym) {
            agg_route_record(AGG_ROUTE_V2_RADIX);
            /* Sparse ranges and excessive dense worker traffic use radix. */
            ray_t* r = exec_group_v2_parallel_radix(g, op, tbl, nrows,
                    key_cols, key_syms, vts, off, block, sel, sel_prefix, n_sel,
                    group_limit);
            agg_vo_free(&vo); scratch_free(kc_hdr); return r;
        }
        /* Hash fallback (F64 / STR keys): not a chunked strategy — compact. */
        if (sel) AGG_RUN_COMPACT_FALLBACK();
        agg_route_record(AGG_ROUTE_V2_INDEXED);
        { ray_t* r = agg_indexed_run(g, op, tbl, key_cols, key_syms, nrows);
          agg_vo_free(&vo); scratch_free(kc_hdr); return r; }
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
    if (grp_rc != 0) { scratch_free(kc_hdr); return ray_error("oom", NULL); }

    ray_t* result = ray_table_new(ext->n_keys + ext->n_aggs);
    if (!result || RAY_IS_ERR(result)) { scratch_free(kc_hdr); agg_groups_free(&groups); return ray_error("oom", NULL); }

    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], groups.first_row, groups.ngroups);
        if (!kc || RAY_IS_ERR(kc)) { scratch_free(kc_hdr); agg_groups_free(&groups); ray_release(result); return kc ? kc : ray_error("oom", NULL); }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }
    scratch_free(kc_hdr);   /* key_cols/key_syms done — agg loop below reads neither */

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
    if (!g || !g->selection)
        return exec_group_v2_run(g, op, tbl, ray_table_nrows(tbl), NULL, NULL, 0,
                                 group_limit);

    int64_t src_nrows = ray_table_nrows(tbl);
    ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
    /* Defensive: a selection that doesn't cover this table's rows can't be
     * applied here — fall back to the unfiltered run (matches the scalar-agg
     * guard in group.c, which also only honors a selection when nrows match). */
    if (sm->nrows != src_nrows)
        return exec_group_v2_run(g, op, tbl, src_nrows, NULL, NULL, 0, group_limit);

    int64_t n_sel = sm->total_pass;
    ray_t* prefix_block = agg_sel_build_prefix(g->selection);
    if (!prefix_block) return ray_error("oom", NULL);
    const int64_t* sel_prefix = (const int64_t*)ray_data(prefix_block);

    /* Hide the selection from the run so a downstream strategy can't double-
     * apply it; the rowsel is passed explicitly via the sel argument instead. */
    ray_t* saved_sel = g->selection;
    g->selection = NULL;
    ray_t* result = exec_group_v2_run(g, op, tbl, src_nrows, saved_sel, sel_prefix,
                                      n_sel, group_limit);
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

/* Write a finalized scalar cell into output column slot i, marking nulls.
 * Shared by agg_run_one (serial) and the parallel finalize. */
static void agg_put_cell(ray_t* out, int64_t i, ray_t* cell) {
    if (agg_put_cell_value(out, i, cell))
        ray_vec_set_null(out, i, true);

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
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL, kparam);
        if (is_list) {
            out = ray_list_set(out, gi, cell);   /* retains cell */
            ray_release(cell);                    /* drop our local ref */
        } else {
            agg_put_cell(out, gi, cell);
            ray_release(cell);
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
        ray_t* cell = vt->finalize(states + (size_t)gi * vt->state_size, NULL, kparam);
        agg_put_cell(out, gi, cell);
        ray_release(cell);
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
    for (int64_t r = 0; r < nrows; r++) {
        int64_t slot = 0;
        for (uint32_t k = 0; k < dp->n_keys; k++)
            slot += agg_dense_component(dp, k, agg_read_key_i64(key_cols[k], data[k], r)) * dp->strides[k];
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
static bool agg_list_key_eq(ray_t* a, ray_t* b) {
    if (!a || !b) return a == b;
    if (a->type == RAY_LIST && b->type == RAY_LIST) {
        if (a->len != b->len) return false;
        for (int64_t i = 0; i < a->len; i++)
            if (!agg_list_key_eq(ray_list_get(a, i), ray_list_get(b, i))) return false;
        return true;
    }
    if (a->type == b->type && (a->type == RAY_SYM || a->type == RAY_STR)) {
        if (a->len != b->len) return false;
        for (int64_t i = 0; i < a->len; i++) {
            if (a->type == RAY_SYM) {
                ray_t* x = ray_sym_domain_str(ray_sym_vec_domain(a), ray_read_sym(ray_data(a), i, a->type, a->attrs));
                ray_t* y = ray_sym_domain_str(ray_sym_vec_domain(b), ray_read_sym(ray_data(b), i, b->type, b->attrs));
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
static uint64_t agg_list_key_hash(ray_t* value) {
    if (!value || ray_is_atom(value)) return ray_atom_hash(value);
    if (value->type == RAY_LIST) {
        uint64_t hash = ray_hash_i64(value->len);
        ray_t* const* children = ray_data(value);
        for (int64_t i = 0; i < value->len; i++) hash = ray_hash_combine(hash, agg_list_key_hash(children[i]));
        return hash;
    }
    if (value->type == RAY_SYM || value->type == RAY_STR) {
        uint64_t hash = ray_hash_i64(value->type);
        for (int64_t i = 0; i < value->len; i++) {
            size_t len = 0; const char* str;
            if (value->type == RAY_SYM) {
                ray_t* atom = ray_sym_domain_str(ray_sym_vec_domain(value), ray_read_sym(ray_data(value), i, value->type, value->attrs));
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
static inline uint64_t agg_key_hash_at(ray_t* col, const void* data, int64_t r) {
    if (col->type == RAY_LIST) return agg_list_key_hash(((ray_t* const*)data)[r]);
    if (col->type == RAY_F32 || col->type == RAY_F64) return ray_hash_i64((int64_t)agg_float_key(col, data, r));
    if (col->type == RAY_GUID) return ray_hash_bytes((const char*)data + (size_t)r * 16, 16);
    if (col->type == RAY_STR) {
        size_t len = 0;
        const char* s = ray_str_vec_get(col, r, &len);
        return ray_hash_bytes(s ? s : "", s ? len : 0);
    }
    return (uint64_t)agg_read_key_i64(col, data, r);
}
static inline int agg_key_eq_at(ray_t* col, const void* data, int64_t a, int64_t b) {
    if (col->type == RAY_LIST) return agg_list_key_eq(((ray_t* const*)data)[a], ((ray_t* const*)data)[b]);
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

int agg_group_keys(ray_t** key_cols, uint32_t n_keys, int64_t nrows, agg_groups_t* out) {
    if (nrows < 0 || nrows > INT32_MAX) return -1;
    /* Unbounded keys: cut-3 lifted both admission gates (the GROUP path and the
     * keys-only DISTINCT path via agg_select_distinct), so the key-data pointer
     * table is an exact carve, not a fixed [16]. */
    ray_t* data_hdr;
    const void** data = (const void**)scratch_alloc(&data_hdr, (size_t)n_keys * sizeof(void*));
    if (!data) return -1;
    for (uint32_t k = 0; k < n_keys; k++) data[k] = ray_data(key_cols[k]);

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
            h ^= agg_key_hash_at(key_cols[k], data[k], r); h *= 1099511628211ULL;
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
                if (!agg_key_eq_at(key_cols[k], data[k], r, fr)) { eq = 0; break; }
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
 * (fixed-width + SYM) via agg_gather_key_col — which adopts a SYM column's source
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
    return agg_gather_key_col(sc, first_row, n);
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
