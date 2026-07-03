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

/**   Shared helpers for exec.c split — included by expr.c, filter.c, join.c, etc.
 *   Small hot-path helpers are static inline; larger functions that remain in
 *   exec.c are declared extern.
 */

#ifndef RAY_EXEC_INTERNAL_H
#define RAY_EXEC_INTERNAL_H

#if !defined(RAY_OS_WINDOWS) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "exec.h"
#include "core/pool.h"
#include "core/profile.h"
#include "store/csr.h"
#include "store/hnsw.h"
#include "lftj.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "table/table.h"
#include "vec/str.h"
#include "vec/vec.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>
#include <ctype.h>

/* ══════════════════════════════════════════
 * Parted segment helpers
 * ══════════════════════════════════════════ */

/* Output attrs for a flat vector gathered/flattened from parted segments:
 * the MAX SYM index width across segments (width bits only).  Partitions
 * legitimately carry different widths — each partition's width tracked
 * the shared vocabulary size at ITS save time, so partitions written
 * after the vocabulary crossed the 255/65535 boundaries are wider than
 * older ones.  The flat output must hold the widest segment's ids. */
static inline uint8_t parted_sym_max_attrs(ray_t** segs, int64_t n_segs) {
    uint8_t w = 0;
    for (int64_t i = 0; i < n_segs; i++)
        if (segs[i] && (segs[i]->attrs & RAY_SYM_W_MASK) > w)
            w = segs[i]->attrs & RAY_SYM_W_MASK;
    return w;
}

/* Copy n cells from flat source vec `src` [src_off..src_off+n) into the raw
 * output buffer `out_data` at [dst_off..), where the output was created
 * with element type `base` and attrs `out_attrs`.  Equal element size
 * (always true for non-SYM bases, and for SYM segments saved at the
 * output's width) takes the memcpy fast path.  A SYM source at a
 * different index width takes the per-cell conversion loop — ids are
 * positions in the shared domain, so widening preserves them verbatim.
 * The loop only runs for genuinely mixed-width parted tables. */
static inline void parted_copy_cells(void* out_data, int8_t base,
                                     uint8_t out_attrs, int64_t dst_off,
                                     ray_t* src, int64_t src_off, int64_t n) {
    uint8_t out_esz = ray_sym_elem_size(base, out_attrs);
    if (base != RAY_SYM ||
        ((src->attrs ^ out_attrs) & RAY_SYM_W_MASK) == 0) {
        memcpy((char*)out_data + (size_t)dst_off * out_esz,
               (const char*)ray_data(src) + (size_t)src_off * out_esz,
               (size_t)n * out_esz);
        return;
    }
    const void* src_data = ray_data(src);
    for (int64_t i = 0; i < n; i++)
        ray_write_sym(out_data, dst_off + i,
                      (uint64_t)ray_read_sym(src_data, src_off + i,
                                             RAY_SYM, src->attrs),
                      RAY_SYM, out_attrs);
}

/* ══════════════════════════════════════════
 * Global profiler
 * ══════════════════════════════════════════ */

extern ray_profile_t g_ray_profile;

/* ══════════════════════════════════════════
 * Arena-based scratch allocation helpers
 * ══════════════════════════════════════════ */

/* Allocate zero-initialized scratch buffer, returns data pointer.
 * *hdr_out receives the ray_t* header for later ray_free(). */
static inline void* scratch_calloc(ray_t** hdr_out, size_t nbytes) {
    ray_t* h = ray_alloc(nbytes);
    if (!h) { *hdr_out = NULL; return NULL; }
    void* p = ray_data(h);
    memset(p, 0, nbytes);
    *hdr_out = h;
    return p;
}

/* Allocate uninitialized scratch buffer. */
static inline void* scratch_alloc(ray_t** hdr_out, size_t nbytes) {
    ray_t* h = ray_alloc(nbytes);
    if (!h) { *hdr_out = NULL; return NULL; }
    *hdr_out = h;
    return ray_data(h);
}

/* Reallocate: alloc new, copy old, free old. Returns new data pointer. */
static inline void* scratch_realloc(ray_t** hdr_out, size_t old_bytes, size_t new_bytes) {
    ray_t* old_h = *hdr_out;
    ray_t* new_h = ray_alloc(new_bytes);
    if (!new_h) return NULL;
    void* new_p = ray_data(new_h);
    if (old_h) {
        memcpy(new_p, ray_data(old_h), old_bytes < new_bytes ? old_bytes : new_bytes);
        ray_free(old_h);
    }
    *hdr_out = new_h;
    return new_p;
}

/* Free a scratch buffer (NULL-safe). */
static inline void scratch_free(ray_t* hdr) {
    if (!hdr) return;
    ray_free(hdr);
}

/* ══════════════════════════════════════════
 * Safe sym intern
 * ══════════════════════════════════════════ */

/* Safe sym intern for constant column names in graph algorithm result tables.
 * Falls back to 0 on failure (column name interning should never fail for
 * short constant strings unless ray_sym_init failed). */
static inline int64_t sym_intern_safe(const char* s, size_t len) {
    int64_t id = ray_sym_intern(s, len);
    return id >= 0 ? id : 0;
}

/* ══════════════════════════════════════════
 * Unified column read/write helpers
 * ══════════════════════════════════════════ */

static inline int64_t read_col_i64(const void* data, int64_t row,
                                    int8_t type, uint8_t attrs) {
    switch (type) {
    case RAY_I64: case RAY_TIMESTAMP:
        return ((const int64_t*)data)[row];
    case RAY_SYM:
        switch (attrs & RAY_SYM_W_MASK) {
        case RAY_SYM_W8:  return (int64_t)((const uint8_t*)data)[row];
        case RAY_SYM_W16: return (int64_t)((const uint16_t*)data)[row];
        case RAY_SYM_W32: return (int64_t)((const uint32_t*)data)[row];
        default:         return ((const int64_t*)data)[row];
        }
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        return (int64_t)((const int32_t*)data)[row];
    case RAY_I16:
        return (int64_t)((const int16_t*)data)[row];
    default: /* RAY_BOOL, RAY_U8 */
        return (int64_t)((const uint8_t*)data)[row];
    }
}

static inline void write_col_i64(void* data, int64_t row, int64_t val,
                                  int8_t type, uint8_t attrs) {
    switch (type) {
    case RAY_I64: case RAY_TIMESTAMP:
        ((int64_t*)data)[row] = val; return;
    case RAY_SYM:
        ray_write_sym(data, row, (uint64_t)val, type, attrs); return;
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        ((int32_t*)data)[row] = (int32_t)val; return;
    case RAY_I16:
        ((int16_t*)data)[row] = (int16_t)val; return;
    default: /* RAY_BOOL, RAY_U8 */
        ((uint8_t*)data)[row] = (uint8_t)val; return;
    }
}

/* ══════════════════════════════════════════
 * RAY_SYM-aware column helpers
 * ══════════════════════════════════════════ */

static inline uint8_t col_esz(const ray_t* col) {
    return ray_sym_elem_size(col->type, col->attrs);
}

/* Fast key reader for DA/sort hot loops: elem_size is pre-computed and
 * loop-invariant, so the switch is always perfectly predicted.  Avoids the
 * ray_read_sym → type dispatch chain (3+ branches per element).
 *
 * Unsigned widening — appropriate for SYM dictionary IDs (which are
 * unsigned) and for packing keys into composite ints where bit-pattern
 * round-trip matters more than sign.  Do NOT use for arithmetic on
 * signed narrow column values (I16/I32/DATE/TIME) — use
 * `read_signed_by_esz` so a stored -1 reads as -1 and not 65535. */
static inline int64_t read_by_esz(const void* data, int64_t row, uint8_t esz) {
    switch (esz) {
    case 1:  return (int64_t)((const uint8_t*)data)[row];
    case 2:  return (int64_t)((const uint16_t*)data)[row];
    case 4:  return (int64_t)((const uint32_t*)data)[row];
    default: return ((const int64_t*)data)[row];
    }
}

/* Sign-aware reader.  Aggregate kernels (SUM/MIN/MAX/AVG) need the
 * stored value's mathematical magnitude, so signed narrow columns
 * must be sign-extended into the int64 result.  Pass `is_unsigned=1`
 * for U8/BOOL or SYM dict ID reads, `0` for I16/I32/I64 and the
 * temporal types (DATE/TIME/TIMESTAMP) which are stored signed. */
static inline int64_t read_signed_by_esz(const void* data, int64_t row,
                                         uint8_t esz, int is_unsigned) {
    if (is_unsigned) {
        switch (esz) {
        case 1:  return (int64_t)((const uint8_t*)data)[row];
        case 2:  return (int64_t)((const uint16_t*)data)[row];
        case 4:  return (int64_t)((const uint32_t*)data)[row];
        default: return ((const int64_t*)data)[row];
        }
    }
    switch (esz) {
    case 1:  return (int64_t)((const int8_t*)data)[row];
    case 2:  return (int64_t)((const int16_t*)data)[row];
    case 4:  return (int64_t)((const int32_t*)data)[row];
    default: return ((const int64_t*)data)[row];
    }
}

static inline ray_t* col_vec_new(const ray_t* src, int64_t cap) {
    if (src->type == RAY_SYM)
        return ray_sym_vec_new(src->attrs & RAY_SYM_W_MASK, cap);
    return ray_vec_new(src->type, cap);
}

/* Propagate str_pool from source to gathered result.
 * Source may be a slice — resolve to owner's pool. */
static inline void col_propagate_str_pool(ray_t* dst, const ray_t* src) {
    if (src->type != RAY_STR || dst->type != RAY_STR) return;
    const ray_t* owner = (src->attrs & RAY_ATTR_SLICE) ? src->slice_parent : src;
    if (owner->str_pool) {
        if (dst->str_pool) ray_release(dst->str_pool);
        ray_retain(owner->str_pool);
        dst->str_pool = owner->str_pool;
    }
}

/* Propagate str_pool from parted segments to gathered result.
 * All segments must share the same pool for memcpy-gathered results
 * to be valid. For multi-pool cases, callers must use the deep-copy
 * gather path (parted_gather_str_col) instead. */
static inline void col_propagate_str_pool_parted(ray_t* dst, ray_t** segs, int64_t n_segs) {
    if (dst->type != RAY_STR) return;
    for (int64_t i = 0; i < n_segs; i++) {
        if (segs[i] && segs[i]->type == RAY_STR && segs[i]->str_pool) {
            col_propagate_str_pool(dst, segs[i]);
            return;
        }
    }
}

/* ---- Null bitmap propagation helpers ---- */

/* Propagate nulls from src to dst via index array: dst[r] gets src's null
 * bit at indices[r].  indices may contain -1 for LEFT/OUTER join fill rows
 * (those are set null unconditionally). */
static inline void col_propagate_nulls_gather(ray_t* dst, const ray_t* src,
                                               const int64_t* indices,
                                               int64_t count) {
    bool src_has_nulls = (src->attrs & RAY_ATTR_HAS_NULLS) != 0;
    for (int64_t r = 0; r < count; r++) {
        if (indices[r] < 0 ||
            (src_has_nulls && ray_vec_is_null((ray_t*)src, indices[r])))
            ray_vec_set_null(dst, r, true);
    }
}

/* Propagate nulls from src[src_off..src_off+count) to dst[dst_off..),
 * for contiguous range copies (HEAD, TAIL, range inserts). */
static inline void col_propagate_nulls_range(ray_t* dst, int64_t dst_off,
                                              const ray_t* src, int64_t src_off,
                                              int64_t count) {
    if (!(src->attrs & RAY_ATTR_HAS_NULLS)) return;
    for (int64_t i = 0; i < count; i++) {
        if (ray_vec_is_null((ray_t*)src, src_off + i))
            ray_vec_set_null(dst, dst_off + i, true);
    }
}

/* Propagate nulls through a boolean filter mask: for each set bit in
 * mask[0..src_len), copy the null bit from src to dst[out_idx++]. */
static inline void col_propagate_nulls_filter(ray_t* dst, const ray_t* src,
                                               const uint8_t* mask,
                                               int64_t src_len) {
    if (!(src->attrs & RAY_ATTR_HAS_NULLS)) return;
    int64_t out = 0;
    for (int64_t i = 0; i < src_len; i++) {
        if (mask[i]) {
            if (ray_vec_is_null((ray_t*)src, i))
                ray_vec_set_null(dst, out, true);
            out++;
        }
    }
}

/* Append one string element from a parted segment, preserving nulls. */
static inline ray_t* parted_str_append_elem(ray_t* out, ray_t* seg,
                                            int64_t local_idx,
                                            const char* pool_base) {
    if ((seg->attrs & RAY_ATTR_HAS_NULLS) && ray_vec_is_null(seg, local_idx)) {
        out = ray_str_vec_append(out, "", 0);
        if (!RAY_IS_ERR(out))
            ray_vec_set_null(out, out->len - 1, true);
    } else {
        ray_str_t* elems = (ray_str_t*)ray_data(seg);
        const char* str = ray_str_t_ptr(&elems[local_idx], pool_base);
        out = ray_str_vec_append(out, str, elems[local_idx].len);
    }
    return out;
}

/* Deep-copy gather from parted RAY_STR segments by row index.
 * Resolves each string from its source segment's pool and appends
 * into the output vector's own pool. Safe for multi-pool segments. */
static inline ray_t* parted_gather_str_rows(ray_t** segs, int64_t n_segs,
                                            const int64_t* row_indices,
                                            int64_t count) {
    /* Build prefix-sum segment boundaries */
    int64_t cumul = 0;
    int64_t stack_ends[64];
    int64_t* seg_ends = (n_segs <= 64) ? stack_ends : NULL;
    ray_t* ends_hdr = NULL;
    if (!seg_ends) {
        seg_ends = (int64_t*)scratch_alloc(&ends_hdr, (size_t)n_segs * sizeof(int64_t));
        if (!seg_ends) return ray_error("oom", NULL);
    }
    for (int64_t i = 0; i < n_segs; i++) {
        cumul += (segs[i]) ? segs[i]->len : 0;
        seg_ends[i] = cumul;
    }

    ray_t* out = ray_vec_new(RAY_STR, count);
    if (!out || RAY_IS_ERR(out)) { if (ends_hdr) scratch_free(ends_hdr); return out; }

    int64_t seg = 0;
    for (int64_t i = 0; i < count; i++) {
        int64_t row = row_indices[i];
        while (seg < n_segs - 1 && row >= seg_ends[seg]) seg++;
        if (!segs[seg]) {
            out = ray_str_vec_append(out, "", 0);
            if (!RAY_IS_ERR(out))
                ray_vec_set_null(out, out->len - 1, true);
        } else {
            int64_t seg_start = (seg > 0) ? seg_ends[seg - 1] : 0;
            int64_t local = row - seg_start;
            const char* pool_base = segs[seg]->str_pool
                                  ? (const char*)ray_data(segs[seg]->str_pool) : NULL;
            out = parted_str_append_elem(out, segs[seg], local, pool_base);
        }
        if (RAY_IS_ERR(out)) { if (ends_hdr) scratch_free(ends_hdr); return out; }
    }
    if (ends_hdr) scratch_free(ends_hdr);
    return out;
}

/* Deep-copy head (first n rows) from parted RAY_STR segments. */
static inline ray_t* parted_head_str(ray_t** segs, int64_t n_segs, int64_t n) {
    ray_t* out = ray_vec_new(RAY_STR, n);
    if (!out || RAY_IS_ERR(out)) return out;
    int64_t remaining = n;
    for (int64_t s = 0; s < n_segs && remaining > 0; s++) {
        if (!segs[s]) continue;
        int64_t seg_len = segs[s]->len;
        int64_t take = (seg_len > remaining) ? remaining : seg_len;
        const char* pool_base = segs[s]->str_pool
                              ? (const char*)ray_data(segs[s]->str_pool) : NULL;
        for (int64_t i = 0; i < take; i++) {
            out = parted_str_append_elem(out, segs[s], i, pool_base);
            if (RAY_IS_ERR(out)) return out;
        }
        remaining -= take;
    }
    return out;
}

/* Deep-copy tail (last n rows) from parted RAY_STR segments. */
static inline ray_t* parted_tail_str(ray_t** segs, int64_t n_segs, int64_t n) {
    /* First pass: count total rows to find start offset */
    int64_t total = 0;
    for (int64_t s = 0; s < n_segs; s++)
        if (segs[s]) total += segs[s]->len;
    int64_t skip = total - n;
    if (skip < 0) { skip = 0; n = total; }

    ray_t* out = ray_vec_new(RAY_STR, n);
    if (!out || RAY_IS_ERR(out)) return out;
    int64_t skipped = 0;
    for (int64_t s = 0; s < n_segs; s++) {
        if (!segs[s]) continue;
        int64_t seg_len = segs[s]->len;
        int64_t seg_start = 0;
        if (skipped + seg_len <= skip) { skipped += seg_len; continue; }
        if (skipped < skip) { seg_start = skip - skipped; skipped = skip; }
        const char* pool_base = segs[s]->str_pool
                              ? (const char*)ray_data(segs[s]->str_pool) : NULL;
        for (int64_t i = seg_start; i < seg_len; i++) {
            out = parted_str_append_elem(out, segs[s], i, pool_base);
            if (RAY_IS_ERR(out)) return out;
        }
        skipped += seg_len;
    }
    return out;
}

/* Deep-copy flatten all rows from parted RAY_STR segments. */
static inline ray_t* parted_flatten_str(ray_t** segs, int64_t n_segs, int64_t total) {
    ray_t* out = ray_vec_new(RAY_STR, total);
    if (!out || RAY_IS_ERR(out)) return out;
    for (int64_t s = 0; s < n_segs; s++) {
        if (!segs[s] || segs[s]->len <= 0) continue;
        const char* pool_base = segs[s]->str_pool
                              ? (const char*)ray_data(segs[s]->str_pool) : NULL;
        for (int64_t i = 0; i < segs[s]->len; i++) {
            out = parted_str_append_elem(out, segs[s], i, pool_base);
            if (RAY_IS_ERR(out)) return out;
        }
    }
    return out;
}

/* Same but from explicit type + attrs (for parted base type, etc.) */
static inline ray_t* typed_vec_new(int8_t type, uint8_t attrs, int64_t cap) {
    if (type == RAY_SYM)
        return ray_sym_vec_new(attrs & RAY_SYM_W_MASK, cap);
    return ray_vec_new(type, cap);
}

/* ══════════════════════════════════════════
 * Cancellation check
 * ══════════════════════════════════════════ */

static inline bool pool_cancelled(ray_pool_t* pool) {
    if (RAY_UNLIKELY(ray_interrupted())) return true;
    return pool && RAY_UNLIKELY(atomic_load_explicit(&pool->cancelled,
                                                     memory_order_relaxed));
}

#define CHECK_CANCEL(pool)                                \
    do { if (pool_cancelled(pool))                        \
             return ray_error("cancel", NULL); } while(0)

#define CHECK_CANCEL_GOTO(pool, lbl)                      \
    do { if (pool_cancelled(pool)) {                      \
             result = ray_error("cancel", NULL);          \
             goto lbl;                                    \
         }                                                \
    } while(0)

/* ══════════════════════════════════════════
 * Graph helpers: ext-node allocation + lookup
 * ══════════════════════════════════════════ */

/* Allocate an ext node (64 B + `extra` trailing bytes for inline arrays).
 * Defined in graph.c.  Used by ray_*-style graph constructors that need
 * to embed variable-length data (keys, agg lists, etc.) after the ext
 * struct.  Use EXT_TRAIL() to find the trailing bytes. */
ray_op_ext_t* graph_alloc_ext_node_ex(ray_graph_t* g, size_t extra);

/* Allocate a plain 32-byte node, growing g->nodes if needed.  Defined in
 * graph.c.  Shared with opt.c (no second copy of the realloc logic). */
ray_op_t* graph_alloc_node(ray_graph_t* g);

/* Compile a Rayfall AST sub-expression to a DAG node.  Defined in
 * query.c.  Used by fused_* operators that need to evaluate a small
 * predicate without going through the full select planner. */
ray_op_t* compile_expr_dag(ray_graph_t* g, ray_t* expr);

/* Pointer to trailing bytes after an ext node. */
#define EXT_TRAIL(ext) ((char*)((ext) + 1))

static inline ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id) {
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == node_id)
            return g->ext_nodes[i];
    }
    return NULL;
}

/* Resolve a node id to its ray_op_t* in g->nodes.  Returns NULL for the
 * RAY_OP_NONE sentinel.  Node ids are stable indices, so the returned
 * pointer is only valid until the next g->nodes realloc — never store it
 * across a node allocation; re-resolve from the id instead. */
static inline ray_op_t* op_node(ray_graph_t* g, uint32_t id) {
    return id == RAY_OP_NONE ? NULL : &g->nodes[id];
}

/* Resolve input edge i (0 or 1) of op to its child node, or NULL if unused. */
static inline ray_op_t* op_child(ray_graph_t* g, const ray_op_t* op, int i) {
    return op_node(g, op->in_id[i]);
}

/* ══════════════════════════════════════════
 * String helpers
 * ══════════════════════════════════════════ */

/* Convert an atom (-RAY_STR or RAY_SYM scalar) to ray_str_t for comparison */
static inline void atom_to_str_t(ray_t* atom, ray_str_t* out, const char** out_pool) {
    const char* sp;
    size_t sl;
    if (atom->type == -RAY_STR) {
        sp = ray_str_ptr(atom);
        sl = ray_str_len(atom);
    } else if (atom->type == RAY_STR) {
        /* Length-1 RAY_STR vector used as scalar */
        if (atom->len < 1) {
            memset(out, 0, sizeof(ray_str_t));
            *out_pool = NULL;
            return;
        }
        /* Resolve slice to parent data — slices have no data of their own,
         * and str_pool shares the union with slice_offset. */
        ray_t* src = atom;
        int64_t idx = 0;
        if (atom->attrs & RAY_ATTR_SLICE) {
            src = atom->slice_parent;
            idx = atom->slice_offset;
        }
        const ray_str_t* elems = (const ray_str_t*)ray_data(src);
        *out = elems[idx];
        *out_pool = src->str_pool ? (const char*)ray_data(src->str_pool) : NULL;
        return;
    } else if (RAY_IS_SYM(atom->type) && ray_is_atom(atom)) {
        /* SAFETY: ray_sym_str returns a borrowed pointer into the append-only
         * sym table.  The pointer is valid for the lifetime of the sym table
         * (i.e., the entire query execution).  If the sym table ever gains
         * eviction, this must retain the returned atom. */
        ray_t* s = ray_sym_str(atom->i64);
        sp = s ? ray_str_ptr(s) : "";
        sl = s ? ray_str_len(s) : 0;
    } else {
        sp = ""; sl = 0;
    }
    memset(out, 0, sizeof(ray_str_t));
    out->len = (uint32_t)sl;
    if (sl <= RAY_STR_INLINE_MAX) {
        if (sl > 0) memcpy(out->data, sp, sl);
        *out_pool = NULL;
    } else {
        memcpy(out->prefix, sp, 4);
        out->pool_off = 0;
        *out_pool = sp; /* point directly at atom's string data */
    }
}

/* Resolve RAY_STR vec to data owner, accounting for slices.
 * Returns element pointer (already offset for slices) and pool pointer. */
static inline void str_resolve(const ray_t* v, const ray_str_t** elems,
                               const char** pool) {
    const ray_t* owner = (v->attrs & RAY_ATTR_SLICE) ? v->slice_parent : v;
    int64_t base = (v->attrs & RAY_ATTR_SLICE) ? v->slice_offset : 0;
    *elems = (const ray_str_t*)ray_data((ray_t*)owner) + base;
    *pool = owner->str_pool ? (const char*)ray_data(owner->str_pool) : NULL;
}

/* Helper: resolve sym/enum element to string.  The id is CELL DATA of
 * `input`, so it resolves through the COLUMN's domain (sym-domain
 * Phase 2) — exact no-op while the domain is the runtime singleton
 * (ray_sym_domain_str delegates to ray_sym_str). */
static inline void sym_elem(const ray_t* input, int64_t i,
                            const char** out_str, size_t* out_len) {
    int64_t sym_id = ray_read_sym(ray_data((ray_t*)input), i, input->type, input->attrs);
    ray_t* atom = ray_sym_domain_str(ray_sym_vec_domain((ray_t*)input), sym_id);
    if (!atom) { *out_str = ""; *out_len = 0; return; }
    *out_str = ray_str_ptr(atom);
    *out_len = ray_str_len(atom);
}

/* ══════════════════════════════════════════
 * Shared types — used by expr.c and exec.c
 * ══════════════════════════════════════════ */

typedef struct {
    bool    enabled;
    double  bias_f64;
    int64_t bias_i64;
} agg_affine_t;

#define AGG_LINEAR_MAX_TERMS 8

typedef struct {
    bool    enabled;
    uint8_t n_terms;
    void*   term_ptrs[AGG_LINEAR_MAX_TERMS];
    int8_t  term_types[AGG_LINEAR_MAX_TERMS];
    int64_t coeff_i64[AGG_LINEAR_MAX_TERMS];
    int64_t bias_i64;
} agg_linear_t;

typedef struct {
    uint8_t n_terms;
    int64_t syms[AGG_LINEAR_MAX_TERMS];
    int64_t coeff_i64[AGG_LINEAR_MAX_TERMS];
    int64_t bias_i64;
} linear_expr_i64_t;

/* SUM/AVG(a * b) fused product input: the group accumulators read both
 * source columns and multiply per row (as F64, exactly the expr path's
 * promote-to-double semantics) instead of materializing the product
 * vector — for a 1M-row group that vector is 8MB of alloc+fault+write
 * per query.  Gated to flat, null-free columns with a float product. */
typedef struct {
    bool        enabled;
    const void* pa; int8_t ta; uint8_t aa;
    const void* pb; int8_t tb; uint8_t ab;
} agg_prod_t;

/* ── Expression compiler types ── */

typedef enum {
    EXPR_BAIL_ROOT = 0, EXPR_BAIL_SIZE, EXPR_BAIL_DEPTH, EXPR_BAIL_REGS,
    EXPR_BAIL_INS, EXPR_BAIL_MAPCOMMON, EXPR_BAIL_STR, EXPR_BAIL_NULLS,
    EXPR_BAIL_SLICE, EXPR_BAIL_SYM_DOMAIN, EXPR_BAIL_CONST,
    EXPR_BAIL_NULL_SHAPE, /* nullable program hit a not-yet-null-capable instruction */
    EXPR_BAIL_OTHER, EXPR_BAIL__N
} expr_bail_t;

extern uint64_t ray_expr_bail_counts[EXPR_BAIL__N];
extern uint64_t ray_expr_compile_ok;
extern bool     ray_expr_disable;
extern bool     ray_opt_no_group_pushdown;
extern bool     ray_join_no_build_swap;
extern uint64_t ray_join_build_swaps;
extern bool     ray_join_force_dup_fallback;
/* perf-gate bypass: disable the auto dup-fallback to measure the pre-fix O(dup²) build */
extern bool     ray_join_no_dup_fallback;
extern uint64_t ray_join_dup_fallbacks;
extern bool     ray_agg_engine_v2; /* route OP_GROUP through v2 agg engine; default off */
void ray_expr_stats_init(void);

#define EXPR_MAX_REGS 16
#define EXPR_MAX_INS  48
#define EXPR_MORSEL   RAY_MORSEL_ELEMS

typedef struct {
    uint8_t opcode;     /* OP_ADD, OP_NEG, OP_CAST, etc. */
    uint8_t dst;        /* destination register */
    uint8_t src1;       /* source 1 register */
    uint8_t src2;       /* source 2 register (0xFF for unary) */
    uint8_t null_aware; /* 1 = use sentinel-checking kernel variant */
} expr_ins_t;

enum { REG_SCAN = 0, REG_CONST = 1, REG_SCRATCH = 2 };

typedef struct {
    uint8_t n_ins;
    uint8_t n_regs;
    uint8_t n_scratch;      /* scratch registers needed */
    uint8_t out_reg;
    int8_t  out_type;       /* RAY_F64, RAY_I64, or RAY_BOOL */
    bool    has_parted;     /* true if any REG_SCAN refs a parted column */
    struct {
        uint8_t     kind;       /* REG_SCAN / REG_CONST / REG_SCRATCH */
        int8_t      type;       /* computational type: RAY_F64 / RAY_I64 / RAY_BOOL */
        int8_t      col_type;   /* original column type (REG_SCAN only) */
        uint8_t     col_attrs;  /* column attrs — RAY_SYM width (REG_SCAN only) */
        bool        is_parted;  /* true if this SCAN refs a parted column */
        bool        nullable;   /* lanes may contain NULL_I64 / NaN */
        const void* data;       /* column data pointer (REG_SCAN only) */
        ray_t*       col_obj;    /* source column vec (REG_SCAN, non-parted) —
                                  * carries the chunk-zone index for zone-skip */
        ray_t*       parted_col; /* parted wrapper (is_parted only) */
        double      const_f64;  /* scalar value (REG_CONST) */
        int64_t     const_i64;  /* scalar value (REG_CONST) */
    } regs[EXPR_MAX_REGS];
    expr_ins_t ins[EXPR_MAX_INS];
} ray_expr_t;

/* ══════════════════════════════════════════
 * Shared gather types — used by filter.c, exec.c (sort, join)
 * ══════════════════════════════════════════ */

#define MGATHER_MAX_COLS 16

typedef struct {
    const int64_t* idx;
    char*          srcs[MGATHER_MAX_COLS];
    char*          dsts[MGATHER_MAX_COLS];
    uint8_t        esz[MGATHER_MAX_COLS];
    int64_t        ncols;
} multi_gather_ctx_t;

typedef struct {
    int64_t*     idx;
    ray_t*        src_col;
    ray_t*        dst_col;
    uint8_t      esz;
    bool         nullable;  /* true = idx may contain -1 (LEFT JOIN nulls) */
} gather_ctx_t;

/* ══════════════════════════════════════════
 * Shared sort types and constants — used by sort_exec.c, exec.c (window)
 * ══════════════════════════════════════════ */

#define RADIX_SORT_THRESHOLD 4096  /* switch from comparison to radix sort */
#define SMALL_POOL_THRESHOLD 8192  /* skip pool dispatch below this size */
#define NEARLY_SORTED_FRAC   0.05  /* threshold for nearly-sorted detection */
#define MK_PRESCAN_MAX_KEYS  8     /* max sort keys for stack allocation */

typedef struct {
    ray_t**       vecs;
    uint8_t*     desc;
    uint8_t*     nulls_first;
    uint8_t      n_sort;
} sort_cmp_ctx_t;

/* Radix pass context (shared across histogram + scatter phases) */
typedef struct {
    const uint64_t*  keys;
    const int64_t*   idx;
    uint64_t*        keys_out;
    int64_t*         idx_out;
    int64_t          n;
    uint8_t          shift;
    uint32_t         n_tasks;
    uint32_t*        hist;       /* flat [n_tasks * 256] */
    int64_t*         offsets;    /* flat [n_tasks * 256] */
} radix_pass_ctx_t;

/* Key-encoding context for parallel encode phase */
typedef struct {
    uint64_t*       keys;      /* output */
    int64_t*        indices;   /* if non-NULL, initialize indices[i]=i (fused iota) */
    /* Single-key fields: */
    const void*     data;      /* raw column data */
    ray_t*          col;       /* source column (for null bitmap access) */
    int8_t          type;      /* column type */
    uint8_t         col_attrs; /* RAY_SYM width attrs */
    bool            desc;
    bool            nulls_first; /* for single-key F64: 1=nulls first */
    /* SYM rank mapping (NULL if not sym): */
    const uint32_t* enum_rank; /* intern_id → sort rank */
    /* Composite-key fields (n_keys > 1): */
    uint8_t         n_keys;
    ray_t**          vecs;
    int64_t         mins[16];
    int64_t         ranges[16];
    uint8_t         bit_shifts[16]; /* bit offset for key k in composite */
    uint8_t         descs[16];
    const uint32_t* enum_ranks[16]; /* per-key rank mappings */
} radix_encode_ctx_t;

/* Parallel multi-key min/max prescan context */
typedef struct {
    ray_t*     const* vecs;
    uint32_t* const* enum_ranks;
    uint8_t          n_keys;
    int64_t          nrows;
    uint32_t         n_workers;
    int64_t*         pw_mins;
    int64_t*         pw_maxs;
} mk_prescan_ctx_t;

/* Parallel sort phase 1 context */
typedef struct {
    const sort_cmp_ctx_t* cmp_ctx;
    int64_t*  indices;
    int64_t*  tmp;
    int64_t   nrows;
    uint32_t  n_chunks;
} sort_phase1_ctx_t;

/* Parallel merge pass context */
typedef struct {
    const sort_cmp_ctx_t* cmp_ctx;
    const int64_t*  src;
    int64_t*        dst;
    int64_t         nrows;
    int64_t         run_size;
} sort_merge_ctx_t;

/* Compute the number of significant bytes for radix sort based on type.
 * Returns 1..8: the number of byte passes radix_sort_run needs. */
static inline uint8_t radix_key_bytes(int8_t type) {
    switch (type) {
    case RAY_BOOL: case RAY_U8:   return 1;
    case RAY_I16:                return 2;
    case RAY_I32: case RAY_DATE: case RAY_TIME: return 4;
    default:                    return 8;  /* I64, F64, TIMESTAMP, SYM */
    }
}

/* ══════════════════════════════════════════
 * Extern forward declarations — larger functions in exec.c
 * ══════════════════════════════════════════ */

/* ── exec.c (gather helpers) ── */
void multi_gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end);
void gather_fn(void* raw, uint32_t wid, int64_t start, int64_t end);
void partitioned_gather(ray_pool_t* pool, const int64_t* idx, int64_t n,
                        int64_t src_rows, char** srcs, char** dsts,
                        const uint8_t* esz, int64_t ncols);

/* ── filter.c ── */
ray_t* exec_filter(ray_graph_t* g, ray_op_t* op, ray_t* input, ray_t* pred);
ray_t* exec_filter_head(ray_t* input, ray_t* pred, int64_t limit);
/* Typed membership kernel (verdict-LUT / SIMD / pool-parallel); NULL when
 * the col/set shape is unsupported — caller falls back to its own path. */
ray_t* ray_in_vec_exec(ray_t* col, ray_t* set, bool negate);
ray_t* sel_compact(ray_graph_t* g, ray_t* tbl, ray_t* sel,
                   const int64_t* keep_syms, int keep_n);

/* ── expr.c ── */
bool try_affine_sumavg_input(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                             ray_t** out_vec, agg_affine_t* out_affine);
bool try_linear_sumavg_input_i64(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                                 agg_linear_t* out_plan);
bool try_prod_sumavg_input_f64(ray_graph_t* g, ray_t* tbl, ray_op_t* input_op,
                               agg_prod_t* out);
bool expr_compile(ray_graph_t* g, ray_t* tbl, ray_op_t* root, ray_expr_t* out);
ray_t* expr_eval_full(const ray_expr_t* expr, int64_t nrows);

/* Fused predicate→selection (skips the full BOOL vec).
 *
 * Compiles and evaluates `pred` in selection-output mode (per-task rowsel
 * builders dispatched over [0,nrows), then finished into one rowsel block).
 * Handles the parted/non-BOOL guard and the
 * large-table fallback (>RAY_POOL_MAX_TASKS tasks → non-morsel-aligned grain).
 * Returns:
 *   - non-NULL rowsel block  → the selection (caller installs / reads it).
 *   - NULL with *all_pass=1  → every row passed (all-pass = "no selection").
 *   - NULL with *all_pass=0  → disabled / unsupported / OOM — caller falls
 *                              back to exec_node→ray_rowsel_from_pred unchanged. */
ray_t* exec_pred_to_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows,
                              bool* all_pass);

/* Per-morsel "fill this morsel's bool" callback used by pred_sel_drive.
 * Writes out[0..n) (1=selected, 0=not) for the kernel's verdict on global
 * rows [start, start+n).  Null rows must resolve to 0 (not-selected). */
typedef void (*pred_sel_fill_fn)(void* ctx, int64_t start, int64_t n,
                                 uint8_t* out);

/* Generic selection-output driver: dispatch `fill` over [0,nrows) in the
 * per-task rowsel-builder model, stitch into one block.  Same tri-state
 * return as exec_pred_to_selection (block / NULL+all_pass / NULL).  Used by
 * the standalone predicate kernels (exec_in_to_selection). */
ray_t* pred_sel_drive(int64_t nrows, pred_sel_fill_fn fill, void* fill_ctx,
                      bool* all_pass);

/* `in` selection-output worker: a root OP_IN over a flat column + literal set,
 * streaming membership bools straight into a rowsel.  Same tri-state return as
 * exec_pred_to_selection; NULL+all_pass=0 for any unsupported shape (non-CONST
 * set, parted/MAPCOMMON/STR column, or OP_NOT_IN) → bool-path fallback. */
ray_t* exec_in_to_selection(ray_graph_t* g, ray_op_t* pred, int64_t nrows,
                            bool* all_pass);
ray_t* exec_elementwise_unary(ray_graph_t* g, ray_op_t* op, ray_t* input);
ray_t* exec_elementwise_binary(ray_graph_t* g, ray_op_t* op, ray_t* lhs, ray_t* rhs);

/* ── sort_exec.c ── */
int sort_cmp(const sort_cmp_ctx_t* ctx, int64_t a, int64_t b);
void sort_insertion(const sort_cmp_ctx_t* ctx, int64_t* arr, int64_t n);
void sort_merge_recursive(const sort_cmp_ctx_t* ctx,
                          int64_t* arr, int64_t* tmp, int64_t n);
void sort_phase1_fn(void* arg, uint32_t worker_id, int64_t start, int64_t end);
void sort_merge_fn(void* arg, uint32_t worker_id, int64_t start, int64_t end);
void key_introsort(uint64_t* keys, int64_t* idx, int64_t n);
double detect_sortedness(ray_pool_t* pool, const uint64_t* keys, int64_t n);
uint8_t compute_key_nbytes(ray_pool_t* pool, const uint64_t* keys,
                            int64_t n, uint8_t type_max);
int64_t* radix_sort_run(ray_pool_t* pool, uint64_t* keys, int64_t* indices,
                         uint64_t* keys_tmp, int64_t* idx_tmp,
                         int64_t n, uint8_t n_bytes,
                         uint64_t** sorted_keys_out);
uint64_t* packed_radix_sort_run(ray_pool_t* pool, uint64_t* data,
                                 uint64_t* tmp, int64_t n, uint8_t n_bytes);
int64_t* msd_radix_sort_run(ray_pool_t* pool, uint64_t* keys, int64_t* indices,
                              uint64_t* keys_tmp, int64_t* idx_tmp,
                              int64_t n, uint8_t n_bytes,
                              uint64_t** sorted_keys_out);
void radix_encode_fn(void* arg, uint32_t wid, int64_t start, int64_t end);
void mk_prescan_fn(void* arg, uint32_t wid, int64_t start, int64_t end);
uint32_t* build_enum_rank(ray_t* col, int64_t nrows, ray_t** hdr_out);
ray_t* exec_sort(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t limit);

/* ── join.c ── */
ray_t* exec_join(ray_graph_t* g, ray_op_t* op, ray_t* left_table, ray_t* right_table);
ray_t* exec_antijoin(ray_graph_t* g, ray_op_t* op,
                     ray_t* left_table, ray_t* right_table);
ray_t* exec_window_join(ray_graph_t* g, ray_op_t* op,
                        ray_t* left_table, ray_t* right_table);

/* ── group.c ── */
ray_t* exec_reduction(ray_graph_t* g, ray_op_t* op, ray_t* input);
ray_t* exec_count_distinct(ray_graph_t* g, ray_op_t* op, ray_t* input);

/* Single-pass per-group count(distinct).  Returns I64 vec of length
 * n_groups, or NULL if `src->type` isn't a supported scalar/SYM type
 * (caller falls back to per-group exec_count_distinct).  Errors are
 * returned as RAY_IS_ERR ray_t*. */
ray_t* ray_count_distinct_per_group(ray_t* src, const int64_t* row_gid,
                                    int64_t n_rows, int64_t n_groups);

/* Parallel exact median per group via ray_pool_dispatch_n.  idx_buf is
 * the group-contiguous row-index layout produced by the upstream
 * group-by phase (already prefix-summed; offsets[g]..offsets[g]+
 * grp_cnt[g] is group g's slice).  Returns F64 vec of n_groups, NULL
 * on unsupported source type (caller falls back to serial). */
ray_t* ray_median_per_group_buf(ray_t* src,
                                const int64_t* idx_buf,
                                const int64_t* offsets,
                                const int64_t* grp_cnt,
                                int64_t n_groups);

/* Parallel per-group bounded top-K / bot-K via ray_pool_dispatch_n.
 * Reuses the same idx_buf/offsets/grp_cnt layout as
 * ray_median_per_group_buf.  K must be >= 1; cells shorter than K when
 * grp_cnt[gi] < K (matches the standalone topk_take_vec convention).
 * desc=1 → top (K largest, descending), desc=0 → bot (K smallest,
 * ascending).  Returns ray_list_new(n_groups), each cell is a vec of
 * the same type as `src`.  NULL on unsupported source type. */
ray_t* ray_topk_per_group_buf(ray_t* src,
                              int64_t k,
                              uint8_t desc,
                              const int64_t* idx_buf,
                              const int64_t* offsets,
                              const int64_t* grp_cnt,
                              int64_t n_groups);

/* Per-group min/max/first/last for wide element types (STR/GUID).  Same
 * idx_buf/offsets/grp_cnt layout as the median/topk kernels; materialises the
 * winning element per group into a typed result column.  Returns NULL for a
 * non-wide src (caller falls back to the numeric path). */
ray_t* ray_wide_minmax_per_group_buf(ray_t* src, uint16_t op,
                                     const int64_t* idx_buf,
                                     const int64_t* offsets,
                                     const int64_t* grp_cnt,
                                     int64_t n_groups);

ray_t* exec_group(ray_graph_t* g, ray_op_t* op, ray_t* tbl, int64_t group_limit);

/* Slice-group fusion probe (exec.c): arm g's slice-group hint instead of
 * executing the WHERE filter when its predicate is exactly in/eq on the
 * single bare group-key column with a fresh CSR hash index.  See the
 * definition for the full contract. */
bool ray_slice_group_probe(ray_graph_t* g, ray_op_t* root, ray_t* by_col);

/* Early hint settle (group.c): fold an armed slice-group hint whose group
 * shape is kernel-ineligible into the equivalent g->selection, ahead of
 * OP_GROUP's sparse pre-compaction.  Returns an error to propagate or
 * NULL. */
ray_t* ray_group_slice_hint_settle(ray_graph_t* g, ray_op_t* op, ray_t* tbl,
                                   int64_t group_limit);

/* Dict-code count-distinct row_gid side-channel: exec_group stashes the
 * per-result-group dict codes so the count-distinct path can build row_gid
 * by remapping codes (cheap int) instead of re-hashing strings. */
typedef struct {
    int32_t*     result_codes;  /* [n_groups] dict code of each result group */
    int64_t      n_groups;
    int64_t      key_sym;
    const ray_t* tbl;
    int64_t      n_distinct;
    uint8_t      valid;
} ray_dict_cd_t;
ray_dict_cd_t ray_dict_cd_get(void);
void ray_dict_cd_clear(void);

/* ── collection.c ── */
ray_t* distinct_vec_eager(ray_t* x);
ray_t* reverse_vec_eager(ray_t* x);

/* ── sort.c ── */
ray_t* asc_vec_eager(ray_t* x);
ray_t* desc_vec_eager(ray_t* x);

/* Group HT types and helpers — shared with pivot (exec.c) */
#define GHT_NEED_SUM     0x01
#define GHT_NEED_MIN     0x02
#define GHT_NEED_MAX     0x04
#define GHT_NEED_SUMSQ   0x08
/* OP_PEARSON_CORR per-group accumulators: x-side piggybacks on SUM and
 * SUMSQ blocks; this flag enables the y-side blocks (Σy, Σy², Σxy). */
#define GHT_NEED_PEARSON 0x10

typedef struct {
    uint16_t entry_stride;
    uint16_t row_stride;
    uint8_t  n_keys;
    uint8_t  n_aggs;
    uint8_t  n_agg_vals;
    uint8_t  need_flags;
    uint8_t  agg_is_f64;
    uint8_t  agg_is_sym;   /* lex compare for MIN/MAX (sym_lex_lt) */
    /* Per-agg SYM resolution domain (sym-domain Phase 2): the domain of
     * agg_vecs[a] when it is a SYM column, NULL otherwise.  Borrowed —
     * the agg input vec outlives every layout use.  accum_from_entry's
     * lex MIN/MAX resolves cell ids through it (cell ids are positions
     * in the COLUMN's domain, not the global intern table). */
    struct ray_sym_domain_s* agg_dom[8];
    uint8_t  agg_is_first;
    uint8_t  agg_is_last;
    uint8_t  agg_is_prod;
    int8_t   agg_val_slot[8];
    uint16_t off_sum;
    uint16_t off_min;
    uint16_t off_max;
    uint16_t off_sumsq;
    /* Per-slot source-row bounds for FIRST/LAST under work-stealing
     * dispatch (see ght_compute_layout).  Allocated only when any agg
     * is OP_FIRST or OP_LAST; otherwise both stay 0.  Each block is
     * n_agg_vals * 8 bytes of int64 (initialized in init_accum_from_entry
     * to the source row index of the entry that opened the group, then
     * monotonically updated by accum_from_entry).  The matching entry
     * tail slot at offset (entry_stride - 8) carries the source-row
     * index of the row the entry was built from. */
    uint16_t off_first_row;
    uint16_t off_last_row;
    /* OP_PEARSON_CORR y-side accumulators.  Allocated when
     * GHT_NEED_PEARSON is set; for an OP_PEARSON_CORR agg at slot s the
     * x-side accumulators live at off_sum[s] (Σx) and off_sumsq[s] (Σx²),
     * the y-side at these three offsets at the same slot index. */
    uint16_t off_sum_y;
    uint16_t off_sumsq_y;
    uint16_t off_sumxy;
    /* Per-agg "binary input" bitset: bit a set iff agg a takes two
     * inputs (OP_PEARSON_CORR).  Drives phase-1 packing — binary aggs
     * pack TWO consecutive 8-byte values per row (x then y) starting at
     * agg_val_slot[a]. */
    uint8_t  agg_is_binary;
    /* Holistic aggregators (OP_MEDIAN): no accumulator slot reserved,
     * agg_val_slot[a] == -1, phase-1 doesn't pack a value, phase-3
     * skips emitting from the row layout.  A separate post-radix pass
     * runs ray_median_per_group_buf over the source column using a
     * row_gid+grp_cnt-derived idx_buf. */
    uint8_t  agg_is_holistic;
    /* Subset of agg_is_holistic: bit a set iff agg a is a wide-element
     * (STR/GUID) min/max/first/last resolved via the per-group winner
     * scan (ray_wide_minmax_per_group_buf) instead of a numeric kernel. */
    uint8_t  agg_is_wide;
    /* Wide-key support: bit k set iff key k does not fit in 8 bytes
     * (e.g. RAY_GUID = 16 B).  For wide keys the 8-byte key slot
     * stores a source-row index and the actual key bytes live in the
     * original column, so probe/rehash/scatter must redirect through
     * key_data[k].  wide_key_esz[k] is the per-element byte size of
     * the source column. */
    uint8_t  wide_key_mask;
    uint8_t  wide_key_esz[8];
    /* Per wide key, the source element type so resolution can branch:
     * RAY_GUID → fixed wide_key_esz bytes; RAY_STR → ray_str_t descriptor
     * (16 B) resolved via the string pool (key_pool[k]) with SSO-aware
     * hash/eq (inline ≤12 B keys need no pool deref). */
    int8_t   wide_key_type[8];
    /* Byte offset of each key within the entry/HT-row key region (relative to
     * the start of the key region, i.e. entry+8 / row+8).  key_off[n_keys] is
     * the null-mask slot.  Most keys are 8 B (key_off[k]==k*8); a RAY_STR key
     * stores its ray_str_t descriptor INLINE (16 B) so probe/rehash compare it
     * cache-locally instead of chasing key_data[k]+row*16 in the source column.
     * key_inline_str bit k set iff key k stores an inline 16 B STR descriptor. */
    uint16_t key_off[9];
    uint16_t key_region;       /* total key region bytes = key_off[n_keys] + 8 */
    uint8_t  key_inline_str;   /* bitset: key k is an inline STR descriptor */
} ght_layout_t;

typedef struct {
    uint32_t*    slots;
    uint32_t     ht_cap;
    char*        rows;
    uint32_t     grp_count;
    uint32_t     grp_cap;
    ght_layout_t layout;
    /* Non-NULL only when layout.wide_key_mask != 0.  Pointers into
     * the original key columns (slice-unaware raw data), used by
     * group_probe_entry / group_ht_rehash to resolve row-indexed
     * wide keys. */
    void*        key_data[8];
    /* String pool base per wide STR key (NULL otherwise) — paired with
     * key_data[k] (the ray_str_t descriptor array) for SSO-aware resolve. */
    const void*  key_pool[8];
    ray_t*        _h_slots;
    ray_t*        _h_rows;
    uint8_t       oom;        /* set by group_probe_entry on grow failure */
} group_ht_t;

/* Row-level accessors for group HT rows */
#define HT_SALT(h)  ((uint8_t)((h) >> 56))
#define HT_EMPTY    UINT32_MAX
#define HT_PACK(salt, gid)  (((uint32_t)(uint8_t)(salt) << 24) | ((gid) & 0xFFFFFF))
#define HT_GID(s)   ((s) & 0xFFFFFF)
#define HT_SALT_V(s) ((uint8_t)((s) >> 24))

#define ROW_RD_F64(row, off, slot) (((const double*)((const void*)((row) + (off))))[(slot)])
#define ROW_RD_I64(row, off, slot) (((const int64_t*)((const void*)((row) + (off))))[(slot)])
#define ROW_WR_F64(row, off, slot) (((double*)((void*)((row) + (off))))[(slot)])
#define ROW_WR_I64(row, off, slot) (((int64_t*)((void*)((row) + (off))))[(slot)])

ght_layout_t ght_compute_layout(uint8_t n_keys, uint8_t n_aggs,
                                ray_t** agg_vecs, uint8_t need_flags,
                                const uint16_t* agg_ops,
                                const int8_t* key_types);
bool group_ht_init(group_ht_t* ht, uint32_t cap, const ght_layout_t* ly);
void group_ht_free(group_ht_t* ht);
typedef struct {
    int enabled;
    uint8_t agg_index;
    int64_t min_count_exclusive;
    int64_t top_count_take;
    /* Agg op of the filtered agg.  When 0 (the default for the
     * historical COUNT-only filter), consumers MUST treat it as
     * OP_COUNT.  When non-zero, must equal ext->agg_ops[agg_index].
     * Supported here: OP_COUNT, OP_SUM, OP_MIN, OP_MAX.  AVG and
     * higher-order aggs (STDDEV/VAR/PEARSON/MEDIAN) are excluded
     * because their ordering doesn't reduce to a single int64 read
     * from the row slot — they fall through to the full sort + take. */
    uint16_t agg_op;
    /* Direction: 1 = top-N largest (desc), 0 = top-N smallest (asc).
     * For COUNT/SUM/MAX the natural ordering is largest-first; for
     * MIN it's smallest-first.  Both directions are supported per
     * agg kind so `desc: min_value take: N` (the N groups with the
     * largest min) is also expressible. */
    uint8_t  desc;
} ray_group_emit_filter_t;
ray_group_emit_filter_t ray_group_emit_filter_get(void);
void ray_group_emit_filter_set(ray_group_emit_filter_t filter);
/* Hash-aggregate rows [start, end) into ht.
 *
 * When match_idx is non-NULL, the loop iterates `i` in [start, end)
 * and reads `row = match_idx[i]` — start/end index the selection
 * space (number of passing rows), not the source column length.
 * When match_idx is NULL, `row = i` — iterating directly over source
 * column rows (no selection). */
/* agg_vecs2 is the optional y-side input column per agg (NULL when no
 * binary aggs).  Pass 1 packs (x, y) consecutively for binary aggs. */
void group_rows_range(group_ht_t* ht, void** key_data, int8_t* key_types,
                      uint8_t* key_attrs, ray_t** key_vecs, ray_t** agg_vecs,
                      ray_t** agg_vecs2,
                      uint8_t* agg_strlen,
                      ray_t* rowsel,
                      int64_t start, int64_t end,
                      const int64_t* match_idx);

/* ══════════════════════════════════════════
 * Pivot ingest — shared parallel hash-aggregate path.
 *
 * Runs the same radix pipeline exec_group uses (phases 1+2), leaving
 * the result in a set of per-partition HTs with prefix offsets. Phase
 * 3 is left to the caller so pivot can restructure the output. For
 * small inputs or when no thread pool is available, falls back to a
 * single sequential HT transparently — the caller iterates
 * part_hts[0..n_parts) the same way either way.
 * ══════════════════════════════════════════ */

typedef struct {
    group_ht_t* part_hts;       /* n_parts entries */
    uint32_t*   part_offsets;   /* n_parts+1 entries (prefix sums of grp_counts) */
    uint32_t    n_parts;        /* 1 when sequential, RADIX_P when parallel */
    uint32_t    total_grps;
    uint16_t    row_stride;

    /* Internal cleanup state — do not touch from callers. */
    ray_t*      _part_hts_hdr;
    ray_t*      _offsets_hdr;
    void*       _radix_bufs;    /* radix_buf_t* — allocated only on parallel path */
    ray_t*      _radix_bufs_hdr;
    size_t      _n_bufs;
} pivot_ingest_t;

/* Run parallel (or sequential-fallback) hash aggregation for pivot.
 * Returns true on success, false on unrecoverable OOM. On true the
 * caller must eventually call pivot_ingest_free(). Cancellation is
 * propagated via ray_interrupted() — callers should check that too. */
bool pivot_ingest_run(pivot_ingest_t* out,
                      const ght_layout_t* ly,
                      void** key_data, int8_t* key_types, uint8_t* key_attrs,
                      ray_t** key_vecs, ray_t** agg_vecs,
                      int64_t n_scan);

void pivot_ingest_free(pivot_ingest_t* out);

/* ── window.c ── */
ray_t* exec_window(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* ── graph_exec.c ── */
ray_t* exec_expand(ray_graph_t* g, ray_op_t* op, ray_t* src_vec);
ray_t* exec_var_expand(ray_graph_t* g, ray_op_t* op, ray_t* start_vec);
ray_t* exec_shortest_path(ray_graph_t* g, ray_op_t* op,
                          ray_t* src_val, ray_t* dst_val);
ray_t* exec_pagerank(ray_graph_t* g, ray_op_t* op);
ray_t* exec_connected_comp(ray_graph_t* g, ray_op_t* op);
ray_t* exec_dijkstra(ray_graph_t* g, ray_op_t* op,
                     ray_t* src_val, ray_t* dst_val);
ray_t* exec_wco_join(ray_graph_t* g, ray_op_t* op);
ray_t* exec_louvain(ray_graph_t* g, ray_op_t* op);
ray_t* exec_degree_cent(ray_graph_t* g, ray_op_t* op);
ray_t* exec_topsort(ray_graph_t* g, ray_op_t* op);
ray_t* exec_cluster_coeff(ray_graph_t* g, ray_op_t* op);
ray_t* exec_betweenness(ray_graph_t* g, ray_op_t* op);
ray_t* exec_closeness(ray_graph_t* g, ray_op_t* op);
ray_t* exec_mst(ray_graph_t* g, ray_op_t* op);
ray_t* exec_random_walk(ray_graph_t* g, ray_op_t* op, ray_t* src_val);
ray_t* exec_dfs(ray_graph_t* g, ray_op_t* op, ray_t* src_val);
ray_t* exec_astar(ray_graph_t* g, ray_op_t* op,
                  ray_t* src_val, ray_t* dst_val);
ray_t* exec_k_shortest(ray_graph_t* g, ray_op_t* op,
                       ray_t* src_val, ray_t* dst_val);

/* ── pivot_exec.c ── */
ray_t* exec_if(ray_graph_t* g, ray_op_t* op);
ray_t* exec_pivot(ray_graph_t* g, ray_op_t* op, ray_t* tbl);

/* ── embedding_exec.c ── */
ray_t* exec_ann_rerank(ray_graph_t* g, ray_op_t* op, ray_t* src);
ray_t* exec_knn_rerank(ray_graph_t* g, ray_op_t* op, ray_t* src);

/* ── temporal_exec.c ── */
ray_t* exec_extract(ray_graph_t* g, ray_op_t* op);
ray_t* exec_date_trunc(ray_graph_t* g, ray_op_t* op);

/* ── string_exec.c ── */
ray_t* exec_like(ray_graph_t* g, ray_op_t* op);
ray_t* exec_ilike(ray_graph_t* g, ray_op_t* op);
ray_t* exec_string_unary(ray_graph_t* g, ray_op_t* op);
ray_t* exec_strlen(ray_graph_t* g, ray_op_t* op);
ray_t* exec_substr(ray_graph_t* g, ray_op_t* op);
ray_t* exec_replace(ray_graph_t* g, ray_op_t* op);
ray_t* exec_concat(ray_graph_t* g, ray_op_t* op);

/* ── exec.c ── */
ray_t* materialize_mapcommon(ray_t* mc);
ray_t* materialize_mapcommon_head(ray_t* mc, int64_t n);
ray_t* materialize_mapcommon_filter(ray_t* mc, ray_t* pred, int64_t pass_count);
ray_t* broadcast_scalar(ray_t* atom, int64_t nrows);
ray_t* exec_node(ray_graph_t* g, ray_op_t* op);

/* ══════════════════════════════════════════
 * Thread-safe null bitmap helpers (parallel group/window)
 * ══════════════════════════════════════════ */

/* Parallel-safe null marker.  Writes the type-correct NULL_* sentinel
 * into payload[idx] and atomically ORs HAS_NULLS into vec->attrs.
 * Payload write needs no synchronisation — different threads call this
 * with different idx, so each per-slot store is uncontended.  attrs OR
 * is atomic so the read-modify-write on the shared attrs byte is safe.
 *
 * BOOL/U8/SYM are non-nullable (rejected at the producer surface) and
 * are no-ops here.  STR/GUID don't appear in parallel aggregation/window
 * output columns and likewise no-op. */
static inline void par_set_null(ray_t* vec, int64_t idx) {
    void* p = ray_data(vec);
    switch (vec->type) {
        case RAY_F64:                          ((double*)p)[idx] = NULL_F64; break;
        case RAY_F32:                          ((float*)p)[idx]  = NULL_F32; break;
        case RAY_I64: case RAY_TIMESTAMP:      ((int64_t*)p)[idx] = NULL_I64; break;
        case RAY_I32: case RAY_DATE: case RAY_TIME: ((int32_t*)p)[idx] = NULL_I32; break;
        case RAY_I16:                          ((int16_t*)p)[idx] = NULL_I16; break;
        default: return;
    }
    __atomic_fetch_or(&vec->attrs, (uint8_t)RAY_ATTR_HAS_NULLS,
                      __ATOMIC_RELAXED);
}

/* Scan payload after parallel execution and set RAY_ATTR_HAS_NULLS if
 * any element carries the type-correct NULL_* sentinel.  This catches
 * the case where par_set_null's atomic OR raced with another thread's
 * load before it took effect — the scan is the post-hoc authoritative
 * check.  No-op for non-sentinel types. */
static inline void par_finalize_nulls(ray_t* vec) {
    int64_t n = vec->len;
    const void* p = ray_data(vec);
    switch (vec->type) {
        case RAY_F64: {
            const double* d = (const double*)p;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != d[i]) { vec->attrs |= RAY_ATTR_HAS_NULLS; return; }
            return;
        }
        case RAY_F32: {
            const float* d = (const float*)p;
            for (int64_t i = 0; i < n; i++)
                if (d[i] != d[i]) { vec->attrs |= RAY_ATTR_HAS_NULLS; return; }
            return;
        }
        case RAY_I64: case RAY_TIMESTAMP: {
            const int64_t* d = (const int64_t*)p;
            for (int64_t i = 0; i < n; i++)
                if (d[i] == NULL_I64) { vec->attrs |= RAY_ATTR_HAS_NULLS; return; }
            return;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME: {
            const int32_t* d = (const int32_t*)p;
            for (int64_t i = 0; i < n; i++)
                if (d[i] == NULL_I32) { vec->attrs |= RAY_ATTR_HAS_NULLS; return; }
            return;
        }
        case RAY_I16: {
            const int16_t* d = (const int16_t*)p;
            for (int64_t i = 0; i < n; i++)
                if (d[i] == NULL_I16) { vec->attrs |= RAY_ATTR_HAS_NULLS; return; }
            return;
        }
        default: return;
    }
}

/* ══════════════════════════════════════════
 * Single-null float model
 * ══════════════════════════════════════════
 * The F64 value domain is {finite} ∪ {0Nf}.  Any non-finite F64 result —
 * NaN OR ±Inf — canonicalizes to NULL_F64 (= __builtin_nan("")) at the
 * produce site; Inf is NOT a value.  Null detection is `x != x`.  Apply
 * ray_f64_fin() to every F64-RESULT produce site (kernels, finalizers,
 * computed-store sites) so a column never holds a non-finite-non-0Nf value.
 *
 * Written compare+select (no early-out branch) so vector loops stay
 * auto-vectorizable.  For DIV/MOD keep the explicit `divisor==0 → NULL_F64`
 * guard at the call site (mirrors the int kernels, avoids the FPU NaN slow
 * path) and wrap only the value-producing branch. */
static inline double ray_f64_fin(double r) {
    /* finite ⟺ |r| <= DBL_MAX: this comparison is FALSE for ±Inf (|Inf| >
     * DBL_MAX) AND for NaN (every NaN compare is false), so it selects exactly
     * the finite values and maps every non-finite to NULL_F64.  Use this fabs +
     * compare + select, NOT __builtin_isfinite — the latter lowers to a scalar
     * fpclassify-style sequence that breaks auto-vectorization and regressed
     * the hot float kernels ~50% (measured).  fabs is a bitwise-AND mask and
     * the ternary becomes a vector blend, keeping the kernel loops vectorized
     * (measured: within noise of baseline). */
    return (__builtin_fabs(r) <= DBL_MAX) ? r : NULL_F64;
}

/* Post-pass for fused/fallback F64 kernels that may have canonicalized a
 * non-finite result to NULL_F64 (NaN) in-buffer: scan [off,off+len) and set
 * RAY_ATTR_HAS_NULLS if any lane is a NaN sentinel.  Mirrors
 * mark_i64_overflow_as_null (which relies on the sentinel already being in
 * the payload — here NULL_F64 is too, so we only flip the attr).  Single
 * pass, memory-bound, branchless inner test; caller invokes single-threaded
 * after pool dispatch joins. */
static inline void mark_f64_nonfinite_as_null(ray_t* result, int64_t off, int64_t len) {
    const double* d = (const double*)ray_data(result) + off;
    for (int64_t i = 0; i < len; i++) {
        if (RAY_UNLIKELY(d[i] != d[i])) { result->attrs |= RAY_ATTR_HAS_NULLS; return; }
    }
}

/* Canonicalise IEEE 754 -0.0 → +0.0 via bit-level check.
 * Immune to -fno-signed-zeros (which makes `if (f==0) f=0` a no-op).
 * Used at output / hash-key boundaries only — not in hot SIMD loops. */
static inline double clear_neg_zero(double v) {
    uint64_t bits; memcpy(&bits, &v, 8);
    if (bits == UINT64_C(0x8000000000000000)) v = 0.0;
    return v;
}

#endif /* RAY_EXEC_INTERNAL_H */
