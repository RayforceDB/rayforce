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

bool ray_agg_engine_v2 = true;   /* knob; default on */

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

bool agg_v2_can_handle(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return false;
    /* Unbounded keys (>=1): every per-run key buffer the v2 engine touches
     * (key_cols/key_syms, the radix scatter's per-worker key row, key_data,
     * agg_group_keys' data[]) is now an exact carve, and the key-count params
     * downstream are uint32_t — so there is no fixed [16]/[255] cap left to
     * protect.  ext->n_keys is uint32_t (widened, Task 1); dense direct-index
     * routing still self-limits to <=16 inside agg_dense_plan (see there). */
    if (ext->n_keys < 1) return false;  /* need >=1 key */
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
    if (!tbl) return false;
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
            if (cnt && cnt->type == RAY_I64) return false;
        }
    }

    /* every key must be a plain column scan of a supported type */
    for (uint32_t k = 0; k < ext->n_keys; k++) {
        ray_op_t* key = op_node(g, ext->keys[k]);
        if (!key || key->opcode != OP_SCAN) return false;
        ray_op_ext_t* kext = find_ext(g, key->id);
        ray_t* kc = kext ? ray_table_get_col(tbl, kext->sym) : NULL;
        if (!kc) return false;
        switch (kc->type) {
            case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
            case RAY_BOOL: case RAY_DATE: case RAY_TIME:
            case RAY_TIMESTAMP: case RAY_SYM: break;
            default: return false;
        }
    }

    /* every aggregate must be a registry-resolvable plain-column scan */
    for (uint32_t a = 0; a < ext->n_aggs; a++) {
        if (ext->agg_k && ext->agg_k[a]) {
            if (ext->agg_ops[a] != OP_TOP_N && ext->agg_ops[a] != OP_BOT_N) return false;
            if (ext->agg_k[a] < 1) return false;
            ray_op_t* in = op_node(g, ext->agg_ins[a]);
            if (!in || in->opcode != OP_SCAN) return false;
            ray_op_ext_t* ie = find_ext(g, in->id);
            ray_t* ic = ie ? ray_table_get_col(tbl, ie->sym) : NULL;
            if (!ic || !agg_resolve(ext->agg_ops[a], ic->type)) return false;
            continue;  /* admitted */
        }
        if (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE) {
            if (!agg_is_binary_agg(ext->agg_ops[a])) return false;
            ray_op_t* xin = op_node(g, ext->agg_ins[a]); ray_op_t* yin = op_node(g, ext->agg_ins2[a]);
            if (!xin || xin->opcode != OP_SCAN || !yin || yin->opcode != OP_SCAN) return false;
            ray_op_ext_t* xe = find_ext(g, xin->id); ray_op_ext_t* ye = find_ext(g, yin->id);
            ray_t* xc = xe ? ray_table_get_col(tbl, xe->sym) : NULL;
            ray_t* yc = ye ? ray_table_get_col(tbl, ye->sym) : NULL;
            if (!xc || !yc) return false;
            if (!agg_resolve(ext->agg_ops[a], xc->type)) return false;
            if (!agg_resolve(ext->agg_ops[a], yc->type)) return false;
            continue;  /* admitted */
        }
        if (ext->agg_ops[a] == OP_COUNT) {
            /* count: needs no typed input column */
            if (!agg_resolve(OP_COUNT, RAY_I64)) return false;
            continue;
        }
        ray_op_t* in = op_node(g, ext->agg_ins[a]);
        if (!in || in->opcode != OP_SCAN) return false;
        ray_op_ext_t* ie = find_ext(g, in->id);
        ray_t* ic = (ie) ? ray_table_get_col(tbl, ie->sym) : NULL;
        if (!ic) return false;
        if (!agg_resolve(ext->agg_ops[a], ic->type)) return false;
    }
    return true;
}

/* ── Dense grouping eligibility selector (mirrors group.c DA path) ────────
 * Decides whether the key tuple packs into a bounded direct-index slot space
 * (gid = sum_k (key_k - min_k)*strides[k]) so grouping can skip hashing.
 * Eligible iff: 1..16 keys, every key is an integer/temporal/SYM type with no
 * nulls, and the product of per-key ranges stays at or below DENSE_MAX_SLOTS
 * (computed overflow-safely).  Performs one min/max prescan per key column.
 * Aggregates may be ACC_STREAMING or ACC_BUFFERED (median/top-k): the dense
 * serial driver (agg_run_one) and the dense parallel path both carry the
 * per-group destroy lifecycle, so buffered state is handled. */
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
        if (kc->attrs & RAY_ATTR_HAS_NULLS) return false;
        if (nrows <= 0) return false;   /* empty → no min/max, max<min guard */

        /* Narrow SYM codes are bounded by their width — codes ∈ [0, 2^w) — so the
         * range is known a priori and ALWAYS within the dense cap (W16 = 65536 <
         * cap).  Skip the O(nrows) min/max prescan entirely; min is 0, range is
         * 2^w.  (W32/W64 still scan: their full code space exceeds the cap, so we
         * need the actual used range to decide dense eligibility.) */
        if (kc->type == RAY_SYM) {
            switch (kc->attrs & RAY_SYM_W_MASK) {
                case RAY_SYM_W8:  out->mins[k] = 0; out->ranges[k] = 256;   continue;
                case RAY_SYM_W16: out->mins[k] = 0; out->ranges[k] = 65536; continue;
                default: break;  /* W32/W64: try domain bounds, else prescan */
            }
            /* W32/W64 SYM codes are positions in [0, domain_count).  When the
             * domain is small enough to stay within the dense array budget (and
             * no larger than the row count), that range is known a priori — skip
             * the O(nrows) min/max prescan, using min=0 / range=count.  Larger
             * domains still scan: their actual used range may be far tighter
             * than the full count, and the array would otherwise blow the cap. */
            int64_t dc = ray_sym_domain_count(ray_sym_vec_domain(kc));
            if (dc > 0 && dc <= 65536 && dc <= nrows) {
                out->mins[k] = 0; out->ranges[k] = dc; continue;
            }
        }

        const void* data = ray_data(kc);
        int64_t mn, mx;
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
        if (mx < mn) return false;       /* no live values */
        out->mins[k]   = mn;
        out->ranges[k] = mx - mn + 1;
    }

    /* Composite packing; bail if a range is non-positive or the product would
     * exceed the cap.  Overflow-safe: check before multiplying. */
    int64_t total = 1;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t rng = out->ranges[k];
        if (rng <= 0) return false;
        if (total > DENSE_MAX_SLOTS / rng) return false;   /* would exceed cap */
        out->strides[k] = total;
        total *= rng;
    }
    if (total > DENSE_MAX_SLOTS) return false;

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
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    size_t esz = col_esz(src_col);
    const char* src = (const char*)ray_data(src_col);
    char* dst = (char*)ray_data(out);
    for (int64_t gi = 0; gi < n; gi++)
        memcpy(dst + (size_t)gi * esz, src + (size_t)first_row[gi] * esz, esz);
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
        d->val_hasnull[a] = vc ? ((vc->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
        d->val_esz[a]     = vc ? col_esz(vc) : 0;
        /* Binary agg (pearson): resolve the y-side column from agg_ins2[a]. */
        ray_t* vc2 = (ext->agg_ins2 && ext->agg_ins2[a] != RAY_OP_NONE)
            ? ray_table_get_col(tbl, find_ext(g, ext->agg_ins2[a])->sym) : NULL;
        d->val2_data[a]    = vc2 ? ray_data(vc2) : NULL;
        d->val2_types[a]   = vc2 ? vc2->type : RAY_I64;
        d->val2_hasnull[a] = vc2 ? ((vc2->attrs & RAY_ATTR_HAS_NULLS) != 0) : false;
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
        if (kc->attrs & RAY_ATTR_HAS_NULLS) return false;
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
                if (v < mn) mn = v;
                if (v > mx) mx = v;
            }
            mins[k] = mn; maxs[k] = mx;
        }
    }
    for (uint32_t k = 0; k < n_keys; k++) {
        if (maxs[k] < mins[k]) { scratch_free(pre_hdr); return false; }
        out->mins[k]   = mins[k];
        out->ranges[k] = maxs[k] - mins[k] + 1;
    }
    scratch_free(pre_hdr);

    int64_t total = 1;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t rng = out->ranges[k];
        if (rng <= 0) return false;
        if (total > DENSE_MAX_SLOTS / rng) return false;
        out->strides[k] = total;
        total *= rng;
    }
    if (total > DENSE_MAX_SLOTS) return false;
    out->total_slots = total;
    out->ok = true;
    return true;
}

/* ══════════════════════════════════════════
 * Parallel two-phase hash group-by (Phase 1c)
 * ══════════════════════════════════════════ */

/* Per-worker (and global merge) local group table: open-addressing hash on the
 * tuple-hash → local gid; AoS per-group state blocks of `block` bytes. */
typedef struct {
    int32_t* ht;          /* [htcap] slot -> local gid, -1 empty */
    int64_t  htcap;
    uint64_t htmask;
    int64_t* first_row;   /* [cap] min row idx per local group */
    char*    states;      /* [cap*block] AoS group state */
    int64_t  ng, cap;
    int      oom;
} agg_local_t;

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

static int agg_local_init(agg_local_t* loc, int64_t cap, int64_t htcap, int64_t block) {
    loc->ng = 0; loc->cap = cap; loc->oom = 0;
    loc->htcap = htcap; loc->htmask = (uint64_t)htcap - 1;
    loc->ht        = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
    loc->first_row = ray_alloc_raw((size_t)cap * sizeof(int64_t));
    loc->states    = ray_alloc_raw((size_t)cap * (size_t)block);
    if (!loc->ht || !loc->first_row || !loc->states) { loc->oom = 1; return -1; }
    for (int64_t i = 0; i < htcap; i++) loc->ht[i] = -1;
    return 0;
}

static void agg_local_destroy(agg_local_t* loc) {
    ray_free_raw(loc->ht); ray_free_raw(loc->first_row); ray_free_raw(loc->states);
    loc->ht = NULL; loc->first_row = NULL; loc->states = NULL;
}

/* Run vt->destroy on every init'd per-group state in `loc` for buffered
 * accumulators (destroy != NULL), then free the slab.  Safe to call once a
 * local/global table's groups have been merged/finalized: streaming accs
 * (destroy == NULL) are skipped, and the call is idempotent on the slab via
 * agg_local_destroy nulling the pointers (but must not be invoked twice on the
 * same live buffered state — each caller below destroys a given table once). */
static void agg_table_destroy(agg_local_t* loc, const agg_vtable_t** vts,
                              const size_t* off, size_t block, uint32_t n_aggs) {
    for (uint32_t a = 0; a < n_aggs; a++)
        if (vts[a]->destroy)
            for (int64_t gg = 0; gg < loc->ng; gg++)
                vts[a]->destroy(loc->states + (size_t)gg * block + off[a]);
    agg_local_destroy(loc);
}

/* Context shared (read-only) across workers; per-worker writes go to locals[wid]. */
typedef struct {
    ray_t**            key_cols;
    const void**       key_data;
    uint32_t           n_keys;
    const agg_vtable_t** vts;
    const size_t*      off;        /* [n_aggs] byte offset of agg a in a block */
    size_t             block;      /* bytes per group block */
    uint32_t           n_aggs;
    const void**       val_data;   /* [n_aggs] base ptr or NULL (COUNT) */
    const int8_t*      val_types;  /* [n_aggs] */
    const bool*        val_hasnull;/* [n_aggs] */
    const uint8_t*     val_esz;    /* [n_aggs] element size or 0 (COUNT) */
    /* Binary aggregates (pearson): second value column.  NULL/0 for unary. */
    const void**       val2_data;  /* [n_aggs] */
    const int8_t*      val2_types; /* [n_aggs] */
    const bool*        val2_hasnull;/* [n_aggs] */
    const uint8_t*     val2_esz;   /* [n_aggs] */
    agg_local_t*       locals;     /* [nw] */
} agg_par_ctx_t;

/* (first_row, group idx) pair.  The radix path packs (part << 32 | local gid)
 * into .idx so finalize can recover both the partition and the in-partition
 * state offset for each emitted group; .fr carries the representative row used
 * to gather the key columns.  Group-by output order is UNSPECIFIED — groups are
 * emitted in build order, so there is no sort. */
typedef struct { int64_t fr; int64_t idx; } agg_fr_pair_t;

/* Phase A: per-worker local group + accumulate over chunk [start,end). */
static void agg_phaseA_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_par_ctx_t* c = (agg_par_ctx_t*)vctx;
    agg_local_t* loc = &c->locals[wid];
    if (loc->oom) return;
    int64_t n = end - start;
    if (n <= 0) return;

    uint32_t* cgid = ray_alloc_raw((size_t)n * sizeof(uint32_t));
    if (!cgid) { loc->oom = 1; return; }

    for (int64_t r = start; r < end; r++) {
        uint64_t h = agg_tuple_hash(c->key_cols, c->key_data, c->n_keys, r);
        uint64_t slot = h & loc->htmask;
        int32_t g;
        for (;;) {
            int32_t gp = loc->ht[slot];
            if (gp < 0) {                       /* new group */
                if (loc->ng == loc->cap) { loc->oom = 1; ray_free_raw(cgid); return; }
                g = (int32_t)loc->ng;
                loc->ht[slot] = g;
                loc->first_row[g] = r;
                for (uint32_t a = 0; a < c->n_aggs; a++)
                    c->vts[a]->init(loc->states + (size_t)g * c->block + c->off[a]);
                loc->ng++;
                break;
            }
            if (agg_tuple_eq(c->key_cols, c->key_data, c->n_keys, r, loc->first_row[gp])) {
                g = gp;
                if (r < loc->first_row[g]) loc->first_row[g] = r;  /* MIN across chunks */
                break;
            }
            slot = (slot + 1) & loc->htmask;
        }
        cgid[r - start] = (uint32_t)g;
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

/* Parallel two-phase path. key_cols/key_syms resolved by caller. */
static ray_t* exec_group_v2_parallel(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block) {
    ray_op_ext_t* ext = find_ext(g, op->id);
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    ray_pool_t* pool = ray_pool_get();
    uint32_t nw = ray_pool_total_workers(pool);

    /* key data bases + per-agg value column bases/types (resolve once). */
    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols)) return ray_error("oom", NULL);

    /* per-worker ht sized to next_pow2(2*nrows): over-allocates (each worker
     * sees only its morsels) but memory isn't gated in 1c — simple & correct. */
    int64_t htcap = 16;
    while (htcap < nrows * 2) htcap <<= 1;
    int64_t cap = nrows > 0 ? nrows : 1;   /* a worker can't exceed nrows groups */

    agg_local_t* locals = ray_calloc_raw((size_t)((size_t)nw) * (sizeof(agg_local_t)));
    if (!locals) { agg_desc_free(&d); return ray_error("oom", NULL); }

    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++)
        if (agg_local_init(&locals[w], cap, htcap, (int64_t)block) != 0) { alloc_oom = 1; break; }
    if (alloc_oom) {
        for (uint32_t w = 0; w < nw; w++) agg_local_destroy(&locals[w]);
        ray_free_raw(locals);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }

    agg_par_ctx_t ctx = {
        .key_cols = key_cols, .key_data = d.key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = d.val_data, .val_types = d.val_types,
        .val_hasnull = d.val_hasnull, .val_esz = d.val_esz,
        .val2_data = d.val2_data, .val2_types = d.val2_types,
        .val2_hasnull = d.val2_hasnull, .val2_esz = d.val2_esz, .locals = locals,
    };
    ray_pool_dispatch(pool, agg_phaseA_fn, &ctx, nrows);

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom) {
            /* Phase A ran: workers may hold init'd buffered group states. */
            for (uint32_t i = 0; i < nw; i++)
                agg_table_destroy(&locals[i], vts, off, block, n_aggs);
            ray_free_raw(locals);
            agg_desc_free(&d);
            return ray_error("oom", NULL);
        }

    /* ── Phase B: merge per-worker locals into a global table (serial) ── */
    agg_local_t gt = {0};
    if (agg_local_init(&gt, cap, htcap, (int64_t)block) != 0) {
        agg_local_destroy(&gt);   /* gt init failed: no init'd states in gt */
        for (uint32_t i = 0; i < nw; i++)
            agg_table_destroy(&locals[i], vts, off, block, n_aggs);
        ray_free_raw(locals);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }

    for (uint32_t w = 0; w < nw; w++) {
        agg_local_t* loc = &locals[w];
        for (int64_t lg = 0; lg < loc->ng; lg++) {
            int64_t fr = loc->first_row[lg];
            uint64_t h = agg_tuple_hash(key_cols, d.key_data, n_keys, fr);
            uint64_t slot = h & gt.htmask;
            int64_t gg;
            for (;;) {
                int32_t gp = gt.ht[slot];
                if (gp < 0) {                          /* new global group */
                    gg = gt.ng;
                    gt.ht[slot] = (int32_t)gg;
                    gt.first_row[gg] = fr;
                    for (uint32_t a = 0; a < n_aggs; a++)
                        vts[a]->init(gt.states + (size_t)gg * block + off[a]);
                    gt.ng++;
                    break;
                }
                if (agg_tuple_eq(key_cols, d.key_data, n_keys, fr, gt.first_row[gp])) {
                    gg = gp;
                    if (fr < gt.first_row[gg]) gt.first_row[gg] = fr;  /* MIN */
                    break;
                }
                slot = (slot + 1) & gt.htmask;
            }
            for (uint32_t a = 0; a < n_aggs; a++)
                vts[a]->merge(gt.states + (size_t)gg * block + off[a],
                              loc->states + (size_t)lg * block + off[a], NULL);
        }
    }
    int64_t ng = gt.ng;

    /* Phase A locals no longer needed past the merge.  Destroy each init'd local
     * per-group state for buffered accumulators (merge copied values into gt) so
     * the local buffers don't leak; do it BEFORE freeing the slabs.  After this
     * point, only gt holds live buffered state — every later path (success +
     * error) destroys gt exactly once and need not touch locals again. */
    for (uint32_t i = 0; i < nw; i++) {
        for (uint32_t a = 0; a < n_aggs; a++)
            if (vts[a]->destroy)
                for (int64_t lg = 0; lg < locals[i].ng; lg++)
                    vts[a]->destroy(locals[i].states + (size_t)lg * block + off[a]);
        agg_local_destroy(&locals[i]);
    }
    ray_free_raw(locals);

    /* ── Phase C: emit key + agg columns in natural global-group order ──
     * Group-by output order is UNSPECIFIED (callers ORDER BY for order), so we
     * emit groups in build/creation order (global hash-slot insertion order)
     * rather than sorting by first_row.  gt.first_row[g] is still the gather
     * index (any member row works) for the key columns. */
    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        agg_table_destroy(&gt, vts, off, block, n_aggs);
        agg_desc_free(&d);
        return result ? result : ray_error("oom", NULL);
    }

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], gt.first_row, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_table_destroy(&gt, vts, off, block, n_aggs);
            agg_desc_free(&d);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint32_t a = 0; a < n_aggs; a++) {
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = is_list ? ray_list_new(ng) : ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            agg_table_destroy(&gt, vts, off, block, n_aggs);
            agg_desc_free(&d);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
        for (int64_t i = 0; i < ng; i++) {
            ray_t* cell = vts[a]->finalize(gt.states + (size_t)i * block + off[a], NULL, kparam);
            if (is_list) {
                out = ray_list_set(out, i, cell);   /* retains cell */
                ray_release(cell);                  /* drop our local ref */
            } else {
                agg_put_cell(out, i, cell);
                ray_release(cell);
            }
        }
        int64_t agg_name = agg_result_col_name(d.agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    agg_table_destroy(&gt, vts, off, block, n_aggs);
    agg_desc_free(&d);
    return result;
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

/* Compute the dense slot for row r via mixed-radix packing. */
static inline int64_t agg_dense_slot(const agg_dense_ctx_t* c, int64_t r) {
    int64_t slot = 0;
    for (uint32_t k = 0; k < c->n_keys; k++)
        slot += (agg_read_key_i64(c->key_cols[k], c->key_data[k], r) - c->dp->mins[k]) * c->dp->strides[k];
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
    if (c->n_keys == 1) {
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

/* Parallel dense path.  Precondition: dp->ok, all aggs ACC_STREAMING, per-worker
 * budget already gated by the caller. */
static ray_t* exec_group_v2_parallel_dense(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl,
        ray_t** key_cols, int64_t* key_syms, ray_op_ext_t* ext, int64_t nrows,
        ray_pool_t* pool, const dense_plan_t* dp,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel) {
    uint32_t n_keys = ext->n_keys, n_aggs = ext->n_aggs;
    int64_t total_slots = dp->total_slots;
    uint32_t nw = ray_pool_total_workers(pool);

    /* Re-derive the AoS layout (same order as exec_group_v2). */
    agg_vo_t vo;
    if (!agg_vo_init(&vo, g, ext, tbl)) return ray_error("oom", NULL);
    const agg_vtable_t** vts = vo.vts; size_t* off = vo.off; size_t block = vo.block;

    agg_desc_t d;
    if (!agg_desc_init(&d, g, ext, tbl, key_cols)) { agg_vo_free(&vo); return ray_error("oom", NULL); }
    const void** key_data = d.key_data;
    const void** val_data = d.val_data; const int8_t* val_types = d.val_types;
    const bool* val_hasnull = d.val_hasnull; const uint8_t* val_esz = d.val_esz;
    const void** val2_data = d.val2_data; const int8_t* val2_types = d.val2_types;
    const bool* val2_hasnull = d.val2_hasnull; const uint8_t* val2_esz = d.val2_esz;
    const int64_t* agg_syms = d.agg_syms;

    agg_dense_local_t* locals = ray_calloc_raw((size_t)((size_t)nw) * (sizeof(agg_dense_local_t)));
    if (!locals) { agg_vo_free(&vo); agg_desc_free(&d); return ray_error("oom", NULL); }
    uint32_t n_init = 0;   /* slabs whose slots were fully init'd (destroy-safe) */
    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++) {
        if (agg_dense_local_init(&locals[w], total_slots, vts, off, block, n_aggs) != 0) { alloc_oom = 1; break; }
        n_init = w + 1;
    }
    if (alloc_oom) {
        /* Only the fully-init'd slabs carry valid (destroyable) buffered state;
         * the slab that failed init never ran its per-slot init → its bytes are
         * uninitialized and must NOT be destroy'd (would free a garbage ptr). */
        for (uint32_t w = 0; w < n_init; w++)
            agg_dense_slab_destroy_states(locals[w].states, total_slots, vts, off, block, n_aggs);
        for (uint32_t w = 0; w < nw; w++) agg_dense_local_destroy(&locals[w]);
        ray_free_raw(locals);
        agg_vo_free(&vo); agg_desc_free(&d);
        return ray_error("oom", NULL);
    }

    agg_dense_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys, .dp = dp,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .locals = locals,
        .sel = sel, .sel_prefix = sel_prefix,
        .vd = { .n_aggs = n_aggs, .vts = vts, .off = off, .block = block,
                .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
                .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz },
    };
    /* Sel mode dispatches over the SELECTED-row space [0,n_sel); each worker
     * range maps to a segment span via sel_prefix.  Non-sel dispatches over rows. */
    ray_pool_dispatch(pool, agg_dense_phaseA_fn, &ctx, sel ? n_sel : nrows);

    for (uint32_t w = 0; w < nw; w++)
        if (locals[w].oom) {
            for (uint32_t i = 0; i < nw; i++)
                agg_dense_slab_destroy_states(locals[i].states, total_slots, vts, off, block, n_aggs);
            for (uint32_t i = 0; i < nw; i++) agg_dense_local_destroy(&locals[i]);
            ray_free_raw(locals);
            agg_vo_free(&vo); agg_desc_free(&d);
            return ray_error("oom", NULL);
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
        return ray_error("oom", NULL);
    }
    for (int64_t s = 0; s < total_slots; s++) {
        gfirst[s] = INT64_MAX;
        for (uint32_t a = 0; a < n_aggs; a++)
            vts[a]->init(gstates + (size_t)s * block + off[a]);
    }
    for (uint32_t w = 0; w < nw; w++) {
        agg_dense_local_t* loc = &locals[w];
        for (int64_t s = 0; s < total_slots; s++) {
            if (loc->first_row[s] == INT64_MAX) continue;   /* untouched by this worker */
            if (loc->first_row[s] < gfirst[s]) gfirst[s] = loc->first_row[s];
            for (uint32_t a = 0; a < n_aggs; a++)
                vts[a]->merge(gstates + (size_t)s * block + off[a],
                              loc->states + (size_t)s * block + off[a], NULL);
        }
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
        return ray_error("oom", NULL);
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
        return result ? result : ray_error("oom", NULL);
    }

    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_gather_key_col(key_cols[k], first_row_ordered, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
            agg_vo_free(&vo); agg_desc_free(&d);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    for (uint32_t a = 0; a < n_aggs; a++) {
        /* Buffered top_n/bot_n produce a LIST cell per group (a native vector);
         * median and all streaming aggs produce a scalar out_type cell. */
        bool is_list = (vts[a]->out_type == RAY_LIST);
        ray_t* out = is_list ? ray_list_new(ng) : ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
            ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
            agg_vo_free(&vo); agg_desc_free(&d);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        int64_t kparam = (ext->agg_k ? ext->agg_k[a] : 0);
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
        int64_t agg_name = agg_result_col_name(agg_syms[a], ext->agg_ops[a]);
        result = ray_table_add_col(result, agg_name, out);
        ray_release(out);
    }

    /* Finalize is done reading the global slab → destroy its buffered per-group
     * state exactly once before freeing the slab. */
    agg_dense_slab_destroy_states(gstates, total_slots, vts, off, block, n_aggs);
    ray_free_raw(occupied_slot); ray_free_raw(first_row_ordered); ray_free_raw(gstates); ray_free_raw(gfirst);
    agg_vo_free(&vo); agg_desc_free(&d);
    return result;
}

/* ══════════════════════════════════════════════════════════════════════
 * Parallel SMALL-HASH group-by (high-reduction sparse int/SYM keys).
 *
 * Targets the shape the dense path rejects (key range exceeds the dense cap)
 * but that still has FEW distinct groups — e.g. 64 groups whose values span
 * 0..6.3e9.  Routing such a shape to RADIX scatters all N rows into 256
 * partitions for a handful of groups (the ~4.7× "scatter tax"); routing it to
 * the generic hash path sizes every per-worker structure to N (htable, states,
 * first_row).  This path is the missing O(groups)-sized strategy: per-worker
 * structures sized to the estimated CARDINALITY, growable on a wrong-low guess.
 *
 * Streaming aggs only (selector gates ACC_BUFFERED → radix).  Per-worker hash:
 * open-addressing, next_pow2(4*estimate) with a small floor, GROWABLE (rehash
 * on load-factor exceed); states + first_row grow with the hash, never with N.
 * Phase A interleaves probe + accumulate in fixed CHUNK_ROWS chunks so the gid
 * buffer is chunk-sized, not nrows-sized.  Phase B merges per-worker hashes into
 * a global hash (≤ groups×nworkers entries — cheap).  Phase C emits in build
 * order (unspecified output order): gather key columns at the representative
 * row per group, finalize each group's streaming state.
 * ══════════════════════════════════════════════════════════════════════ */

/* Cardinality estimate via a bounded sample of the first rows.  Inserts key
 * tuples into a small open-addressing hash (reusing agg_tuple_hash/agg_tuple_eq
 * for byte-consistency with the real grouping) and EARLY-EXITS the moment the
 * distinct count exceeds t_route → returns t_route+1 to signal "high card" so
 * high-card shapes (q10/S2) pay only the bounded sample, never a full pass.
 * On no early-exit, returns the exact distinct count of the sample (capped at
 * t_route).  sample_rows is clamped to nrows.  n_keys is uint32_t (unbounded:
 * carries the untruncated ext->n_keys). */
static int64_t agg_estimate_card(ray_t** key_cols, const void** key_data,
                                 uint32_t n_keys, int64_t nrows,
                                 int64_t sample_rows, int64_t t_route) {
    int64_t ns = sample_rows < nrows ? sample_rows : nrows;
    if (ns <= 0) return 0;
    /* Hash sized to comfortably hold t_route distinct keys at <0.5 load. */
    int64_t htcap = 16;
    while (htcap < (t_route + 1) * 4) htcap <<= 1;
    uint64_t htmask = (uint64_t)htcap - 1;
    int32_t* ht = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
    int64_t* rep = ray_alloc_raw((size_t)(t_route + 2) * sizeof(int64_t)); /* gid -> sample row */
    if (!ht || !rep) { ray_free_raw(ht); ray_free_raw(rep); return t_route + 1; /* assume high */ }
    for (int64_t i = 0; i < htcap; i++) ht[i] = -1;
    int64_t distinct = 0;
    for (int64_t r = 0; r < ns; r++) {
        uint64_t h = agg_tuple_hash(key_cols, key_data, n_keys, r);
        uint64_t slot = h & htmask;
        for (;;) {
            int32_t gp = ht[slot];
            if (gp < 0) {
                ht[slot] = (int32_t)distinct;
                rep[distinct] = r;
                distinct++;
                if (distinct > t_route) { ray_free_raw(ht); ray_free_raw(rep); return t_route + 1; }
                break;
            }
            if (agg_tuple_eq(key_cols, key_data, n_keys, r, rep[gp])) break;
            slot = (slot + 1) & htmask;
        }
    }
    ray_free_raw(ht); ray_free_raw(rep);
    return distinct;
}

/* Sel-mode cardinality estimate: samples the first `sample_rows` SELECTED rows
 * (decoded to ORIGINAL indices via the cursor) so the routing reflects the
 * cardinality of the rows that actually survive the WHERE — not the full table.
 * Same early-exit-above-t_route contract as agg_estimate_card.  n_keys is
 * uint32_t (unbounded), same contract as agg_estimate_card above. */
static int64_t agg_estimate_card_sel(ray_t** key_cols, const void** key_data,
                                     uint32_t n_keys, ray_t* sel,
                                     const int64_t* sel_prefix, int64_t n_sel,
                                     int64_t sample_rows, int64_t t_route) {
    int64_t ns = sample_rows < n_sel ? sample_rows : n_sel;
    if (ns <= 0) return 0;
    int64_t htcap = 16;
    while (htcap < (t_route + 1) * 4) htcap <<= 1;
    uint64_t htmask = (uint64_t)htcap - 1;
    int32_t* ht = ray_alloc_raw((size_t)htcap * sizeof(int32_t));
    int64_t* rep = ray_alloc_raw((size_t)(t_route + 2) * sizeof(int64_t));
    if (!ht || !rep) { ray_free_raw(ht); ray_free_raw(rep); return t_route + 1; }
    for (int64_t i = 0; i < htcap; i++) ht[i] = -1;
    int64_t distinct = 0, seen = 0;
    int64_t rows[AGG_SEL_CHUNK];
    agg_sel_cursor_t cur;
    agg_sel_cursor_init(&cur, sel, sel_prefix, 0, ns);
    int64_t cn;
    while (seen < ns && (cn = agg_sel_cursor_next(&cur, rows)) > 0) {
        for (int64_t i = 0; i < cn && seen < ns; i++, seen++) {
            int64_t r = rows[i];
            uint64_t h = agg_tuple_hash(key_cols, key_data, n_keys, r);
            uint64_t slot = h & htmask;
            for (;;) {
                int32_t gp = ht[slot];
                if (gp < 0) {
                    ht[slot] = (int32_t)distinct; rep[distinct] = r; distinct++;
                    if (distinct > t_route) { ray_free_raw(ht); ray_free_raw(rep); return t_route + 1; }
                    break;
                }
                if (agg_tuple_eq(key_cols, key_data, n_keys, r, rep[gp])) break;
                slot = (slot + 1) & htmask;
            }
        }
    }
    ray_free_raw(ht); ray_free_raw(rep);
    return distinct;
}

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
    int64_t htcap = 16;
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

/* Adaptive promotion: the cardinality estimate is sample-based and can
 * underestimate on clustered/ordered data, misrouting a genuinely high-card
 * group to this small-hash path where the per-worker hash thrashes cache.
 * Guard against it WITHOUT trusting the estimate: when any worker's live group
 * count crosses this threshold the per-worker hash no longer fits cache, so we
 * abort and re-run on the radix path (data-driven, no sampling bias).  Set well
 * above the smallhash/radix crossover so medium-card groups are never promoted. */
#define AGG_SH_PROMOTE (1 << 16)
static ray_t* exec_group_v2_parallel_radix(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel);

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
    int64_t            est_card;   /* sizing hint for per-worker hashes */
    agg_sh_t*          locals;     /* [nw] */
    _Atomic(int)       overflow;   /* set when any worker crosses AGG_SH_PROMOTE */
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
            /* Adaptive promotion trip-wire: too many distinct keys for cache, or
             * another worker already tripped → abort to the radix path. */
            if (sh->ng > AGG_SH_PROMOTE) atomic_store_explicit(&c->overflow, 1, memory_order_relaxed);
            if (atomic_load_explicit(&c->overflow, memory_order_relaxed)) { agg_sel_scratch_free(&sc); return; }
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
        /* Adaptive promotion trip-wire (see AGG_SH_PROMOTE): per-worker hash no
         * longer fits cache, or another worker already tripped → abort so the
         * caller re-runs on radix. */
        if (sh->ng > AGG_SH_PROMOTE) atomic_store_explicit(&c->overflow, 1, memory_order_relaxed);
        if (atomic_load_explicit(&c->overflow, memory_order_relaxed)) return;
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

/* Parallel small-hash path.  Precondition: int/SYM keys, all aggs ACC_STREAMING,
 * estimated cardinality LOW (caller-gated).  est_card sizes the per-worker
 * hashes; they grow if the estimate proves too low. */
static ray_t* exec_group_v2_parallel_smallhash(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        int64_t est_card, ray_t* sel, const int64_t* sel_prefix, int64_t n_sel) {
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

    /* Per-worker hash sized to ~4× the estimated cardinality (floor 256) — the
     * cap, not N.  Grows by rehash if a worker sees more distinct keys than the
     * sample predicted (rare-key tail).  Cap is bounded by nrows for safety. */
    int64_t cap0 = est_card * 4;
    if (cap0 < 256) cap0 = 256;
    if (nrows > 0 && cap0 > nrows) cap0 = nrows;

    agg_sh_t* locals = ray_calloc_raw((size_t)((size_t)nw) * (sizeof(agg_sh_t)));
    if (!locals) { agg_desc_free(&d); return ray_error("oom", NULL); }
    int alloc_oom = 0;
    for (uint32_t w = 0; w < nw; w++)
        if (agg_sh_init(&locals[w], cap0, block) != 0) { alloc_oom = 1; break; }
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
        .est_card = est_card, .locals = locals, .overflow = 0,
        .sel = sel, .sel_prefix = sel_prefix,
        .vd = { .n_aggs = n_aggs, .vts = vts, .off = off, .block = block,
                .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
                .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz },
    };
    ray_pool_dispatch(pool, agg_sh_phaseA_fn, &ctx, sel ? n_sel : nrows);

    /* Adaptive promotion: a worker found the live group count too high for the
     * per-worker hash (estimate underestimated).  Discard the partial small-hash
     * state and re-run on the radix path — data-driven, no sampling bias. */
    if (atomic_load_explicit(&ctx.overflow, memory_order_relaxed)) {
        for (uint32_t w = 0; w < nw; w++) agg_sh_destroy(&locals[w]);
        ray_free_raw(locals);
        agg_desc_free(&d);
        return exec_group_v2_parallel_radix(g, op, tbl, nrows, key_cols, key_syms,
                                            vts, off, block, sel, sel_prefix, n_sel);
    }

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
 * Partition rows by key-hash into AGG_RADIX_P disjoint partitions: every row
 * whose tuple-hash maps to partition p lands in p across ALL workers (same key
 * → same hash → same partition).  Partitions therefore hold DISJOINT key sets,
 * so each can be grouped+accumulated fully in parallel with NO cross-partition
 * merge — eliminating the serial Phase-B bottleneck of the hash path.
 *
 *   Phase 1 (parallel over rows): scatter each row's INDEX into a per-(worker,
 *            partition) growable int64 buffer keyed by RADIX_PART(hash).
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

#define AGG_RADIX_P 256

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
static char* agg_pay_reserve(agg_pay_buf_t* b, size_t rec) {
    if (b->n == b->cap) {
        uint32_t nc = b->cap ? b->cap * 2 : 64;
        char* nb = ray_realloc_raw(b->buf, (size_t)nc * rec);
        if (!nb) return NULL;
        b->buf = nb; b->cap = nc;
    }
    return b->buf + (size_t)b->n++ * rec;
}

/* Per-partition result slot (filled by Phase 2). */
typedef struct {
    char*    states;     /* [ng * block] AoS group state (every group init'd) */
    int64_t* first_row;  /* [ng] MIN row idx per partition-local group */
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

/* Build a result key column of src_col's type for the radix path by un-packing
 * the per-group packed key value (column key_idx of each group's representative
 * record) SEQUENTIALLY from the per-partition `keys` buffers, in emit order
 * (partition-major, then partition-local gid).  This replaces the scattered
 * gather of the original key column at first_row[gi] — for huge ngroups the
 * sequential read is far more cache-friendly.
 *
 * agg_read_key_i64 widened each key into int64 (sign-extend for signed types,
 * zero-extend for U8/BOOL/SYM intern ids); write_col_i64 is its exact inverse,
 * so the round-trip stores byte-identical payload to what agg_gather_key_col's
 * raw memcpy of the original column produced — for SYM it routes through
 * ray_write_sym at the matching width, and the domain is adopted from src_col
 * just like agg_gather_key_col.  Caller owns the returned column.
 * key_idx/n_keys stay uint8_t: both are admission-bounded <=16 (see
 * agg_v2_can_handle) and key_idx indexes the packed-key stride below. */
static ray_t* agg_unpack_key_col(ray_t* src_col, uint32_t key_idx, uint32_t n_keys,
                                 const agg_radix_part_t* parts, uint32_t nparts,
                                 int64_t n) {
    ray_t* out = col_vec_new(src_col, n);
    if (!out || RAY_IS_ERR(out)) return out;
    if (out->type == RAY_SYM)
        ray_sym_vec_adopt_domain(out, sym_domain_rep(src_col));
    out->len = n;
    int8_t type = src_col->type; uint8_t attrs = src_col->attrs;
    void* dst = ray_data(out);
    int64_t i = 0;
    for (uint32_t p = 0; p < nparts; p++) {
        const int64_t* keys = parts[p].keys;
        int64_t png = parts[p].ng;
        for (int64_t gg = 0; gg < png; gg++, i++)
            write_col_i64(dst, i, keys[(size_t)gg * n_keys + key_idx], type, attrs);
    }
    if (src_col->attrs & RAY_ATTR_HAS_NULLS) out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
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
    agg_pay_buf_t*      bufs;       /* [nw * AGG_RADIX_P] payload records */
    agg_radix_part_t*   parts;      /* [AGG_RADIX_P] */
    int                 phase1_oom; /* set by any Phase-1 worker on push failure */
    /* Per-row payload record layout (bytes), computed once by the caller:
     *   [0 .. n_keys*8)         packed keys (each widened via agg_read_key_i64)
     *   [val_off[a] ..]         agg a's input value at native esz (if val_data[a])
     *   [val2_off[a] ..]        agg a's 2nd input (pearson) at native esz
     *   [row_off ..]            int64 source row index (for first_row) */
    size_t              rec;        /* total record size, 8-aligned */
    const size_t*       val_off;    /* [n_aggs], carved by the caller */
    const size_t*       val2_off;   /* [n_aggs], carved by the caller */
    size_t              row_off;
    /* When false, NO admitted aggregate consults the original row (first_row is
     * dead): the record omits its 8-byte row index and Phase-2/3 skip it.  Set
     * once from the agg kinds; row-dependent aggs (FIRST/LAST) flip it true. */
    bool                needs_row;
    /* Sel-mode (pushed WHERE filter): when sel != NULL, scatter the SELECTED
     * rows of [0,n_sel) (decoded to ORIGINAL row indices) instead of [start,end).
     * The packed record's row_off ALWAYS stores the ORIGINAL decoded row, so
     * first_row / key-unpack stay correct in original-row space. */
    ray_t*              sel;
    const int64_t*      sel_prefix;
    /* Per-WORKER key-staging row: [nw * n_keys] int64.  The scatter reads all
     * keys into its worker's slice to compute the partition hash, THEN copies
     * them into the reserved record (the record's partition is not known until
     * the hash is computed, so the keys can't be written straight to it).  One
     * carve for the whole dispatch (caller-owned) — never a per-row alloc. */
    int64_t*            kv_scratch;
} agg_radix_ctx_t;

/* Scatter one ORIGINAL row r's packed payload record into the worker's per-
 * partition buffer.  Returns 0 or -1 on push OOM (sets phase1_oom). */
static inline int agg_radix_scatter_one(agg_radix_ctx_t* c, agg_pay_buf_t* my,
                                        int64_t* kv, int64_t r) {
    uint32_t n_keys = c->n_keys, n_aggs = c->n_aggs;
    /* kv: this worker's key-staging slice (c->kv_scratch + wid*n_keys), sized
     * for any key count — no fixed [16] cap. */
    uint64_t h = 1469598103934665603ULL;
    for (uint32_t k = 0; k < n_keys; k++) {
        int64_t v = agg_read_key_i64(c->key_cols[k], c->key_data[k], r);
        kv[k] = v;
        h ^= (uint64_t)v; h *= 1099511628211ULL;
    }
    uint32_t p = (uint32_t)(h & (AGG_RADIX_P - 1));
    char* rec = agg_pay_reserve(&my[p], c->rec);
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
    if (c->needs_row) *(int64_t*)(rec + c->row_off) = r;  /* loop-invariant branch */
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
        int64_t cn;
        while ((cn = agg_sel_cursor_next(&cur, rows)) > 0)
            for (int64_t i = 0; i < cn; i++)
                if (agg_radix_scatter_one(c, my, kv, rows[i]) != 0) return;
        return;
    }
    for (int64_t r = start; r < end; r++)
        if (agg_radix_scatter_one(c, my, kv, r) != 0) return;
}

/* Phase 1: scatter one packed payload record per row into per-(worker,
 * partition) contiguous buffers, keyed by RADIX_PART(tuple hash). */
static void agg_radix_scatter_fn(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    agg_radix_ctx_t* c = (agg_radix_ctx_t*)vctx;
    agg_pay_buf_t* my = &c->bufs[(size_t)wid * AGG_RADIX_P];
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
            total += c->bufs[(size_t)w * AGG_RADIX_P + p].n;
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
            agg_pay_buf_t* b = &c->bufs[(size_t)w * AGG_RADIX_P + p];
            const char* rec = b->buf;
            for (uint32_t i = 0; i < b->n; i++, rec += c->rec) {
                const int64_t* keys = (const int64_t*)rec;
                int64_t r = c->needs_row ? *(const int64_t*)(rec + c->row_off) : 0;
                /* Gather this row's agg values into the dense per-agg buffers
                 * (sequential record read → sequential dense write). */
                for (uint32_t a = 0; a < n_aggs; a++) {
                    if (gv[a]) { uint8_t ez = c->val_esz[a];
                        memcpy(gv[a] + (size_t)ri * ez, rec + c->val_off[a], ez); }
                    if (gy[a]) { uint8_t ez2 = c->val2_esz[a];
                        memcpy(gy[a] + (size_t)ri * ez2, rec + c->val2_off[a], ez2); }
                }
                /* Hash the contiguous packed keys (same FNV-1a as the scatter). */
                uint64_t h = 1469598103934665603ULL;
                for (uint32_t k = 0; k < n_keys; k++) { h ^= (uint64_t)keys[k]; h *= 1099511628211ULL; }
                uint64_t slot = h & htmask;
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
typedef struct {
    agg_radix_part_t*    parts;
    const agg_fr_pair_t* pairs;       /* [ng], sorted by first_row */
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
        case RAY_F64: ((double*)ray_data(out))[i]  = is_null ? NULL_F64 : cell->f64; break;
        default:      ((int64_t*)ray_data(out))[i] = is_null ? NULL_I64 : cell->i64; break;
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
            ray_t* cell = c->vts[a]->finalize(
                c->parts[p].states + (size_t)gg * c->block + c->off[a], NULL, kparam);
            if (agg_put_cell_value(out, i, cell)) any_null = true;
            ray_release(cell);
        }
        if (any_null) c->saw_null[(size_t)wid * n_aggs + a] = 1;
    }
}

static ray_t* exec_group_v2_parallel_radix(
        ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t nrows,
        ray_t** key_cols, int64_t* key_syms,
        const agg_vtable_t** vts, const size_t* off, size_t block,
        ray_t* sel, const int64_t* sel_prefix, int64_t n_sel) {
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

    /* Row-dependent aggregates (FIRST/LAST) consult the representative original
     * row; every other admitted agg (COUNT/SUM/AVG/MIN/MAX/VAR/STDDEV/PEARSON/
     * MEDIAN/TOP/BOT) accumulates from gathered values and never touches it, and
     * the key columns are un-packed from the packed keys — so first_row/row_idx
     * are dead weight.  Drop them from the record layout when none is needed.
     * NOTE: today FIRST/LAST never reach this kernel (agg_resolve() returns NULL
     * for them, so the v2 admission gate rejects them), so needs_row is always
     * false here — the scan below is a forward-guard, NOT a working restore path:
     * admitting a row-dependent agg would also require finalize wiring to read
     * the row, not just this layout flag. */
    bool needs_row = false;
    for (uint32_t a = 0; a < n_aggs; a++)
        if (ext->agg_ops[a] == OP_FIRST || ext->agg_ops[a] == OP_LAST) { needs_row = true; break; }

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

    size_t nbuf = (size_t)nw * AGG_RADIX_P;
    agg_pay_buf_t*    bufs  = ray_calloc_raw((size_t)(nbuf) * (sizeof(agg_pay_buf_t)));
    agg_radix_part_t* parts = ray_calloc_raw((size_t)(AGG_RADIX_P) * (sizeof(agg_radix_part_t)));
    if (!bufs || !parts) {
        ray_free_raw(bufs); ray_free_raw(parts);
        scratch_free(off_hdr); agg_desc_free(&d);
        return ray_error("oom", NULL);
    }

    agg_radix_ctx_t ctx = {
        .key_cols = key_cols, .key_data = key_data, .n_keys = n_keys,
        .vts = vts, .off = off, .block = block, .n_aggs = n_aggs,
        .val_data = val_data, .val_types = val_types, .val_hasnull = val_hasnull, .val_esz = val_esz,
        .val2_data = val2_data, .val2_types = val2_types, .val2_hasnull = val2_hasnull, .val2_esz = val2_esz,
        .nw = nw, .bufs = bufs, .parts = parts, .phase1_oom = 0,
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
        ray_pool_dispatch_n(pool, agg_radix_group_fn, &ctx, AGG_RADIX_P);
    if (!oom)
        for (uint32_t p = 0; p < AGG_RADIX_P; p++)
            if (parts[p].oom) { oom = 1; break; }

    /* Phases 1+2 done (both dispatches joined); val_off/val2_off no longer read. */
    scratch_free(off_hdr);

    if (oom) {
        agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }

    /* Phase 3: emit in natural partition/group order. ──
     * Group-by output order is UNSPECIFIED, so we emit groups in partition order
     * then partition-local build order (no global sort by first_row).  The
     * (part<<32 | gid) idx encoding locates each group's state during finalize;
     * key columns are un-packed sequentially from the contiguous packed-key
     * buffers (agg_unpack_key_col), not gathered from scattered first_row[]. */
    int64_t ng = 0;
    for (uint32_t p = 0; p < AGG_RADIX_P; p++) ng += parts[p].ng;

    /* (first_row, part, local gid) collected globally in build order.  idx packs
     * (part << 32 | local gid) so finalize recovers both the partition and the
     * in-partition state offset. */
    agg_fr_pair_t* pairs = ray_alloc_raw((size_t)(ng > 0 ? ng : 1) * sizeof(agg_fr_pair_t));
    if (!pairs) {
        agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return ray_error("oom", NULL);
    }
    { int64_t i = 0;
      for (uint32_t p = 0; p < AGG_RADIX_P; p++)
          for (int64_t gg = 0; gg < parts[p].ng; gg++) {
              pairs[i].fr  = parts[p].first_row ? parts[p].first_row[gg] : 0;  /* unused by emit */
              pairs[i].idx = ((int64_t)p << 32) | (uint32_t)gg;   /* part | gid */
              i++;
          }
    }

    ray_t* result = ray_table_new(n_keys + n_aggs);
    if (!result || RAY_IS_ERR(result)) {
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
        for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
        ray_free_raw(bufs); ray_free_raw(parts);
        agg_desc_free(&d);
        return result ? result : ray_error("oom", NULL);
    }

    /* Emit key columns by sequential un-pack from the contiguous per-partition
     * packed-key buffers (cache-friendly), NOT a scattered gather of the
     * original columns at first_row[].  Byte-identical to agg_gather_key_col
     * (see agg_unpack_key_col), incl. SYM payload + domain. */
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* kc = agg_unpack_key_col(key_cols[k], k, n_keys, parts, AGG_RADIX_P, ng);
        if (!kc || RAY_IS_ERR(kc)) {
            ray_free_raw(pairs);
            agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
            for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
            ray_free_raw(bufs); ray_free_raw(parts);
            agg_desc_free(&d);
            ray_release(result); return kc ? kc : ray_error("oom", NULL);
        }
        result = ray_table_add_col(result, key_syms[k], kc);
        ray_release(kc);
    }

    /* Pre-allocate every agg's output column up front so the parallel finalize
     * pass can write disjoint slices into all scalar columns at once.  LIST
     * (top/bot) columns are finalized serially below — outs[a] stays NULL for
     * them so the parallel worker skips them. */
    ray_t* outs_hdr;
    ray_t** outs = (ray_t**)scratch_calloc(&outs_hdr, (size_t)n_aggs * (sizeof(ray_t*) + sizeof(int64_t)));
    if (!outs) {
        ray_free_raw(pairs);
        agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
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
        ray_t* out = is_list ? ray_list_new(ng) : ray_vec_new(vts[a]->out_type, ng);
        if (!out || RAY_IS_ERR(out)) {
            for (uint32_t b = 0; b < a; b++) ray_release(outs[b]);
            ray_free_raw(pairs);
            agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
            for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
            ray_free_raw(bufs); ray_free_raw(parts);
            scratch_free(outs_hdr); agg_desc_free(&d);
            ray_release(result); return out ? out : ray_error("oom", NULL);
        }
        out->len = ng;
        outs[a] = out;
        kparams[a] = (ext->agg_k ? ext->agg_k[a] : 0);
    }

    /* Parallelize the scalar finalize over the output rows when ngroups is large
     * (the q10/high-card win).  Small ng (incl. every low-card shape) keeps the
     * trivial serial loop → zero dispatch overhead, no regression. */
    bool par_fin = (pool && ng >= RAY_PARALLEL_THRESHOLD);
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
            ray_pool_dispatch(pool, agg_radix_finalize_fn, &fc, ng);
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
            for (int64_t i = 0; i < ng; i++) {
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
    agg_radix_parts_destroy(parts, AGG_RADIX_P, vts, off, block, n_aggs);
    for (size_t i = 0; i < nbuf; i++) ray_free_raw(bufs[i].buf);
    ray_free_raw(bufs); ray_free_raw(parts);
    scratch_free(outs_hdr); agg_desc_free(&d);
    return result;
}

static ray_t* agg_build_compact(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                int64_t* idx, int64_t n_sel);

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
                                const int64_t* sel_prefix, int64_t n_sel) {
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
            ray_t* r = exec_group_v2_run(g, op, compact, n_sel, NULL, NULL, 0); \
            ray_release(compact); ray_release(idxb);                            \
            return r;                                                           \
        } while (0)

    /* Effective row count for the parallel-vs-serial decision: the selected
     * count when a filter is active, else the full table. */
    int64_t eff_n = sel ? n_sel : nrows;

    ray_pool_t* pool = ray_pool_get();
    if (pool && eff_n >= RAY_PARALLEL_THRESHOLD) {
        /* Per-worker memory budget gate: dense parallel allocates a full
         * total_slots*(block + first_row) slab per worker.  Low-card (the perf
         * target) has tiny total_slots → always within budget → dense parallel.
         * Mid-card-over-budget dense plans fall back to hash parallel (correct). */
        /* Compute per_worker_bytes ONLY when the dense plan is valid: a bailed
         * plan (dp.ok == false) leaves dp.total_slots unset, and the multiply
         * would overflow on garbage (UBSan signed-overflow).  Short-circuit on
         * dp.ok first. */
        bool dense_par_ok = false;
        if (dp.ok) {
            int64_t per_worker_bytes = dp.total_slots * (int64_t)(block + 8 /*first_row*/);
            dense_par_ok = per_worker_bytes <= (8LL << 20);  /* 8 MB/worker cap */
        }

        /* RADIX eligibility: every key an int/SYM type with no nulls (same
         * type-set check as agg_dense_plan).  Radix takes the high-card
         * remainder of int-key queries (dense handles the low-card head),
         * for BOTH streaming and buffered (median/top) aggs — radix runs the
         * full init/update_batch/finalize/destroy lifecycle.  Hash handles
         * F64 / STR keys. */
        bool keys_intsym = true;
        for (uint32_t k = 0; k < ext->n_keys && keys_intsym; k++) {
            ray_t* kc = key_cols[k];
            switch (kc->type) {
                case RAY_I64: case RAY_I32: case RAY_I16: case RAY_U8:
                case RAY_BOOL: case RAY_DATE: case RAY_TIME:
                case RAY_TIMESTAMP: case RAY_SYM: break;
                default: keys_intsym = false;
            }
            if (kc->attrs & RAY_ATTR_HAS_NULLS) keys_intsym = false;
        }

        /* All aggregates streaming?  (Buffered median/top-k keep radix — its
         * per-group destroy lifecycle handles them; the small-hash path is
         * streaming-only.) */
        bool all_streaming = true;
        for (uint32_t a = 0; a < ext->n_aggs && all_streaming; a++)
            if (vts[a]->kind != ACC_STREAMING) all_streaming = false;

        if (dense_par_ok) {
            ray_t* r = exec_group_v2_parallel_dense(g, op, tbl, key_cols, key_syms, ext, nrows, pool, &dp,
                                                    sel, sel_prefix, n_sel);
            agg_vo_free(&vo); scratch_free(kc_hdr); return r;
        }
        if (keys_intsym) {
            /* Dense-FAIL int/SYM streaming shapes: probe cardinality on a bounded
             * sample.  LOW card → small-hash (O(groups) working set, no scatter
             * tax); HIGH card → radix.  The estimate early-exits above T_route so
             * high-card shapes pay only the bounded sample.  In sel mode the
             * sample is taken over the SELECTED rows (the cardinality that
             * actually reaches grouping), so a high-selectivity filter over a
             * high-card column still routes to small-hash when the surviving
             * rows are few-and-low-card. */
            if (all_streaming) {
                /* Exact-size carve (unbounded keys) for the estimate's key-data
                 * pointers; freed at the smallhash return and on fall-through. */
                ray_t* kd_hdr;
                const void** key_data = (const void**)scratch_alloc(&kd_hdr, (size_t)ext->n_keys * sizeof(void*));
                if (!key_data) { agg_vo_free(&vo); scratch_free(kc_hdr); return ray_error("oom", NULL); }
                for (uint32_t k = 0; k < ext->n_keys; k++) key_data[k] = ray_data(key_cols[k]);
                /* T_ROUTE: the group cardinality where the small-hash
                 * per-worker working set stops fitting cache and radix's
                 * partitioned scatter wins.  Small-hash keeps one hash
                 * table of `groups` entries per worker (≈ groups ×
                 * (key + state) bytes) and probes it per input row; while
                 * that table stays resident in fast cache the probes are
                 * near-free and small-hash beats radix's extra
                 * partition/scatter pass.  Once `groups` grows past cache
                 * residency the probes start missing to DRAM and per-row
                 * cost climbs steeply, whereas radix is cardinality-flat
                 * (it partitions keys into cache-sized buckets up front).
                 * Measured 1-key 10M-row sweep on this class of machine:
                 * the streaming-SUM crossover lands right at ~16384 groups
                 * (smallhash ≈ radix there; smallhash ~2x faster below,
                 * ~2x slower above), which is ~0.4 MB of per-worker table —
                 * the L2-residency edge.  Lighter aggs (COUNT) cross a bit
                 * lower; since radix degrades gracefully past the crossover
                 * while small-hash blows up, we sit at the high end of that
                 * band (16384) so a misroute costs little. */
                const int64_t T_ROUTE = 16384;
                int64_t est = sel
                    ? agg_estimate_card_sel(key_cols, key_data, ext->n_keys, sel, sel_prefix, n_sel, 65536, T_ROUTE)
                    : agg_estimate_card(key_cols, key_data, ext->n_keys, nrows, 65536, T_ROUTE);
                if (est <= T_ROUTE) {
                    ray_t* r = exec_group_v2_parallel_smallhash(g, op, tbl, nrows,
                            key_cols, key_syms, vts, off, block, est, sel, sel_prefix, n_sel);
                    scratch_free(kd_hdr); agg_vo_free(&vo); scratch_free(kc_hdr); return r;
                }
                scratch_free(kd_hdr);   /* fall through to radix */
            }
            { ray_t* r = exec_group_v2_parallel_radix(g, op, tbl, nrows, key_cols, key_syms, vts, off, block,
                                                      sel, sel_prefix, n_sel);
              agg_vo_free(&vo); scratch_free(kc_hdr); return r; }
        }
        /* Hash fallback (F64 / STR keys): not a chunked strategy — compact. */
        if (sel) AGG_RUN_COMPACT_FALLBACK();
        { ray_t* r = exec_group_v2_parallel(g, op, tbl, nrows, key_cols, key_syms, vts, off, block);
          agg_vo_free(&vo); scratch_free(kc_hdr); return r; }
    }

    /* Serial path does not consult the vts/off tables (per-agg vt is re-resolved
     * below), so release the layout carve before it runs. */
    agg_vo_free(&vo);

    /* Serial path: keep the compact fallback when a filter is active (selected
     * count is below the parallel threshold here → small; not a perf blocker). */
    if (sel) AGG_RUN_COMPACT_FALLBACK();

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
ray_t* exec_group_v2(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!g || !g->selection)
        return exec_group_v2_run(g, op, tbl, ray_table_nrows(tbl), NULL, NULL, 0);

    int64_t src_nrows = ray_table_nrows(tbl);
    ray_rowsel_t* sm = ray_rowsel_meta(g->selection);
    /* Defensive: a selection that doesn't cover this table's rows can't be
     * applied here — fall back to the unfiltered run (matches the scalar-agg
     * guard in group.c, which also only honors a selection when nrows match). */
    if (sm->nrows != src_nrows)
        return exec_group_v2_run(g, op, tbl, src_nrows, NULL, NULL, 0);

    int64_t n_sel = sm->total_pass;
    ray_t* prefix_block = agg_sel_build_prefix(g->selection);
    if (!prefix_block) return ray_error("oom", NULL);
    const int64_t* sel_prefix = (const int64_t*)ray_data(prefix_block);

    /* Hide the selection from the run so a downstream strategy can't double-
     * apply it; the rowsel is passed explicitly via the sel argument instead. */
    ray_t* saved_sel = g->selection;
    g->selection = NULL;
    ray_t* result = exec_group_v2_run(g, op, tbl, src_nrows, saved_sel, sel_prefix, n_sel);
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
    switch (out->type) {
        case RAY_F64:
            ((double*)ray_data(out))[i] = cell->f64; break;
        default: /* RAY_I64 (and temporal widths if they arise) */
            ((int64_t*)ray_data(out))[i] = cell->i64; break;
    }
    if (RAY_ATOM_IS_NULL(cell))
        ray_vec_set_null(out, i, true);   /* also sets RAY_ATTR_HAS_NULLS */
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
                          val_col ? ((val_col->attrs & RAY_ATTR_HAS_NULLS) != 0) : false };
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
                       (x_col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
    ray_valid_t vy = { ray_data(y_col), y_col->type,
                       (y_col->attrs & RAY_ATTR_HAS_NULLS) != 0 };
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
            slot += (agg_read_key_i64(key_cols[k], data[k], r) - dp->mins[k]) * dp->strides[k];
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
static inline uint64_t agg_key_hash_at(ray_t* col, const void* data, int64_t r) {
    if (col->type == RAY_STR) {
        size_t len = 0;
        const char* s = ray_str_vec_get(col, r, &len);
        return ray_hash_bytes(s ? s : "", s ? len : 0);
    }
    return (uint64_t)agg_read_key_i64(col, data, r);
}
static inline int agg_key_eq_at(ray_t* col, const void* data, int64_t a, int64_t b) {
    if (col->type == RAY_STR) {
        size_t la = 0, lb = 0;
        const char* sa = ray_str_vec_get(col, a, &la);
        const char* sb = ray_str_vec_get(col, b, &lb);
        return la == lb && (la == 0 || memcmp(sa, sb, la) == 0);
    }
    return agg_read_key_i64(col, data, a) == agg_read_key_i64(col, data, b);
}

int agg_group_keys(ray_t** key_cols, uint32_t n_keys, int64_t nrows, agg_groups_t* out) {
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
