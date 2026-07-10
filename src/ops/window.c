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
#include "lang/internal.h"  /* sym_cell_runtime_id (sym-domain Phase 2) */

/* ============================================================================
 * Window function execution
 * ============================================================================ */

/* Compare rows ra and rb on the given key columns. Returns true if any differ. */
static inline bool win_keys_differ(ray_t* const* vecs, uint32_t n_keys,
                                    int64_t ra, int64_t rb) {
    for (uint32_t k = 0; k < n_keys; k++) {
        ray_t* col = vecs[k];
        if (!col) continue;
        switch (col->type) {
        case RAY_I64: case RAY_TIMESTAMP:
            if (((const int64_t*)ray_data(col))[ra] !=
                ((const int64_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_F64: {
            double a = ((const double*)ray_data(col))[ra];
            double b = ((const double*)ray_data(col))[rb];
            if (a != b) return true;
            break;
        }
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            if (((const int32_t*)ray_data(col))[ra] !=
                ((const int32_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_SYM:
            /* Partition equality of two rows of ONE column: both ids
             * live in the same domain, raw index compare is correct
             * for any domain (sym-domain Phase 2 — no change).  Same
             * holds for the pkey gathers below.  The win_read_* SYM
             * reads are different: their ids LEAVE the column into
             * I64/F64 outputs, so they re-express through the cell's
             * domain into runtime ids (see win_read_i64/f64). */
            if (ray_read_sym(ray_data(col), ra, col->type, col->attrs) !=
                ray_read_sym(ray_data(col), rb, col->type, col->attrs)) return true;
            break;
        case RAY_I16:
            if (((const int16_t*)ray_data(col))[ra] !=
                ((const int16_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_BOOL: case RAY_U8:
            if (((const uint8_t*)ray_data(col))[ra] !=
                ((const uint8_t*)ray_data(col))[rb]) return true;
            break;
        case RAY_STR: {
            const ray_str_t* elems;
            const char* pool;
            str_resolve(col, &elems, &pool);
            if (!ray_str_t_eq(&elems[ra], pool, &elems[rb], pool)) return true;
            break;
        }
        default: break;
        }
    }
    return false;
}

/* Value readers for lag/lead/first_value/last_value/nth_value and the
 * numeric frame aggregates.  A SYM cell's id LEAVES its column here
 * (into an I64/F64 result vec), so it must be re-expressed as a RUNTIME
 * id through the cell's domain (sym-domain Phase 2) — raw pass-through
 * when the column is runtime-domain (exact no-op pre-flip). */
static inline double win_read_f64(ray_t* col, int64_t row) {
    switch (col->type) {
    case RAY_F64: return ((const double*)ray_data(col))[row];
    case RAY_I64: case RAY_TIMESTAMP:
        return (double)((const int64_t*)ray_data(col))[row];
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        return (double)((const int32_t*)ray_data(col))[row];
    case RAY_SYM:
        return (double)sym_cell_runtime_id(col, row);
    case RAY_I16: return (double)((const int16_t*)ray_data(col))[row];
    case RAY_BOOL: case RAY_U8: return (double)((const uint8_t*)ray_data(col))[row];
    default: return 0.0;
    }
}

static inline int64_t win_read_i64(ray_t* col, int64_t row) {
    switch (col->type) {
    case RAY_I64: case RAY_TIMESTAMP:
        return ((const int64_t*)ray_data(col))[row];
    case RAY_I32: case RAY_DATE: case RAY_TIME:
        return (int64_t)((const int32_t*)ray_data(col))[row];
    case RAY_SYM:
        return sym_cell_runtime_id(col, row);
    case RAY_F64: return (int64_t)((const double*)ray_data(col))[row];
    case RAY_I16: return (int64_t)((const int16_t*)ray_data(col))[row];
    case RAY_BOOL: case RAY_U8: return (int64_t)((const uint8_t*)ray_data(col))[row];
    default: return 0;
    }
}

/* Aliases for shared parallel null helpers from internal.h */
#define win_set_null       par_set_null
#define win_finalize_nulls par_finalize_nulls

/* Resolve a graph op node to a column vector from tbl */
static ray_t* win_resolve_vec(ray_graph_t* g, ray_op_t* key_op, ray_t* tbl,
                              uint8_t* owned) {
    ray_op_ext_t* key_ext = find_ext(g, key_op->id);
    if (key_ext && key_ext->base.opcode == OP_SCAN) {
        *owned = 0;
        return ray_table_get_col(tbl, key_ext->sym);
    }
    *owned = 1;
    ray_t* saved = g->table;
    g->table = tbl;
    ray_t* v = exec_node(g, key_op);
    g->table = saved;
    return v;
}

/* Compute window functions for one partition [ps, pe) in sorted_idx */
static void win_compute_partition(
    ray_t* const* order_vecs, uint32_t n_order,
    ray_t* const* func_vecs, const uint8_t* func_kinds, const int64_t* func_params,
    uint32_t n_funcs,
    uint8_t frame_start, uint8_t frame_end, int64_t frame_start_n,
    const int64_t* sorted_idx, int64_t ps, int64_t pe,
    ray_t* const* result_vecs, const bool* is_f64)
{
    if (ps >= pe) return; /* empty partition — nothing to compute */
    int64_t part_len = pe - ps;

    for (uint32_t f = 0; f < n_funcs; f++) {
        uint8_t kind = func_kinds[f];
        ray_t* fvec = func_vecs[f];
        ray_t* rvec = result_vecs[f];
        bool whole = (frame_start == RAY_BOUND_UNBOUNDED_PRECEDING &&
                      frame_end == RAY_BOUND_UNBOUNDED_FOLLOWING);
        bool trailing = (frame_start == RAY_BOUND_N_PRECEDING &&
                         frame_end == RAY_BOUND_CURRENT_ROW);

        switch (kind) {
        case RAY_WIN_ROW_NUMBER: {
            int64_t* out = (int64_t*)ray_data(rvec);
            for (int64_t i = ps; i < pe; i++)
                out[sorted_idx[i]] = i - ps + 1;
            break;
        }
        case RAY_WIN_RANK: {
            int64_t* out = (int64_t*)ray_data(rvec);
            int64_t rank = 1;
            out[sorted_idx[ps]] = 1;
            for (int64_t i = ps + 1; i < pe; i++) {
                if (n_order > 0 && win_keys_differ(order_vecs, n_order,
                        sorted_idx[i-1], sorted_idx[i]))
                    rank = i - ps + 1;
                out[sorted_idx[i]] = rank;
            }
            break;
        }
        case RAY_WIN_DENSE_RANK: {
            int64_t* out = (int64_t*)ray_data(rvec);
            int64_t rank = 1;
            out[sorted_idx[ps]] = 1;
            for (int64_t i = ps + 1; i < pe; i++) {
                if (n_order > 0 && win_keys_differ(order_vecs, n_order,
                        sorted_idx[i-1], sorted_idx[i]))
                    rank++;
                out[sorted_idx[i]] = rank;
            }
            break;
        }
        case RAY_WIN_NTILE: {
            int64_t n = func_params[f];
            if (n <= 0) n = 1;
            int64_t* out = (int64_t*)ray_data(rvec);
            for (int64_t i = ps; i < pe; i++)
                out[sorted_idx[i]] = ((i - ps) * n) / part_len + 1;
            break;
        }
        case RAY_WIN_COUNT: {
            int64_t* out = (int64_t*)ray_data(rvec);
            if (whole) {
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = part_len;
            } else if (trailing) {
                int64_t width = frame_start_n + 1;
                for (int64_t i = ps; i < pe; i++) {
                    int64_t seen = i - ps + 1;
                    out[sorted_idx[i]] = seen < width ? seen : width;
                }
            } else {
                for (int64_t i = ps; i < pe; i++)
                    out[sorted_idx[i]] = i - ps + 1;
            }
            break;
        }
        case RAY_WIN_SUM: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    double total = 0.0;
                    for (int64_t i = ps; i < pe; i++)
                        if (!ray_vec_is_null(fvec, sorted_idx[i]))
                            total += win_read_f64(fvec, sorted_idx[i]);
                    total = ray_f64_fin(total);
                    for (int64_t i = ps; i < pe; i++) out[sorted_idx[i]] = total;
                } else {
                    double acc = 0.0;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t row = sorted_idx[i];
                        if (!ray_vec_is_null(fvec, row)) acc += win_read_f64(fvec, row);
                        if (trailing) {
                            int64_t drop = i - frame_start_n - 1;
                            if (drop >= ps && !ray_vec_is_null(fvec, sorted_idx[drop]))
                                acc -= win_read_f64(fvec, sorted_idx[drop]);
                        }
                        out[row] = ray_f64_fin(acc);
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    int64_t total = 0;
                    for (int64_t i = ps; i < pe; i++)
                        if (!ray_vec_is_null(fvec, sorted_idx[i]))
                            total += win_read_i64(fvec, sorted_idx[i]);
                    for (int64_t i = ps; i < pe; i++) out[sorted_idx[i]] = total;
                } else {
                    int64_t acc = 0;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t row = sorted_idx[i];
                        if (!ray_vec_is_null(fvec, row)) acc += win_read_i64(fvec, row);
                        if (trailing) {
                            int64_t drop = i - frame_start_n - 1;
                            if (drop >= ps && !ray_vec_is_null(fvec, sorted_idx[drop]))
                                acc -= win_read_i64(fvec, sorted_idx[drop]);
                        }
                        out[row] = acc;
                    }
                }
            }
            break;
        }
        case RAY_WIN_AVG: {
            if (!fvec) break;
            double* out = (double*)ray_data(rvec);
            if (whole) {
                double total = 0.0;
                int64_t count = 0;
                for (int64_t i = ps; i < pe; i++) {
                    int64_t row = sorted_idx[i];
                    if (!ray_vec_is_null(fvec, row)) {
                        total += win_read_f64(fvec, row);
                        count++;
                    }
                }
                if (count > 0) {
                    double avg = ray_f64_fin(total / (double)count);
                    for (int64_t i = ps; i < pe; i++) out[sorted_idx[i]] = avg;
                } else {
                    for (int64_t i = ps; i < pe; i++) win_set_null(rvec, sorted_idx[i]);
                }
            } else {
                double acc = 0.0;
                int64_t count = 0;
                for (int64_t i = ps; i < pe; i++) {
                    int64_t row = sorted_idx[i];
                    if (!ray_vec_is_null(fvec, row)) {
                        acc += win_read_f64(fvec, row);
                        count++;
                    }
                    if (trailing) {
                        int64_t drop = i - frame_start_n - 1;
                        if (drop >= ps && !ray_vec_is_null(fvec, sorted_idx[drop])) {
                            acc -= win_read_f64(fvec, sorted_idx[drop]);
                            count--;
                        }
                    }
                    if (count > 0) out[row] = ray_f64_fin(acc / (double)count);
                    else win_set_null(rvec, row);
                }
            }
            break;
        }
        case RAY_WIN_MIN:
        case RAY_WIN_MAX: {
            if (!fvec) break;
            bool want_max = kind == RAY_WIN_MAX;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    double best = want_max ? -DBL_MAX : DBL_MAX;
                    int found = 0;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t row = sorted_idx[i];
                        if (ray_vec_is_null(fvec, row)) continue;
                        double v = win_read_f64(fvec, row);
                        if (!found || (want_max ? v > best : v < best)) { best = v; found = 1; }
                    }
                    for (int64_t i = ps; i < pe; i++) {
                        if (found) out[sorted_idx[i]] = best;
                        else win_set_null(rvec, sorted_idx[i]);
                    }
                } else {
                    if (!trailing) {
                        double best = want_max ? -DBL_MAX : DBL_MAX;
                        int found = 0;
                        for (int64_t i = ps; i < pe; i++) {
                            int64_t row = sorted_idx[i];
                            if (!ray_vec_is_null(fvec, row)) {
                                double v = win_read_f64(fvec, row);
                                if (!found || (want_max ? v > best : v < best)) { best = v; found = 1; }
                            }
                            if (found) out[row] = best;
                            else win_set_null(rvec, row);
                        }
                    } else {
                        for (int64_t i = ps; i < pe; i++) {
                            int64_t lo = i - frame_start_n;
                            if (lo < ps) lo = ps;
                            double best = want_max ? -DBL_MAX : DBL_MAX;
                            int found = 0;
                            for (int64_t j = lo; j <= i; j++) {
                                int64_t row = sorted_idx[j];
                                if (ray_vec_is_null(fvec, row)) continue;
                                double v = win_read_f64(fvec, row);
                                if (!found || (want_max ? v > best : v < best)) { best = v; found = 1; }
                            }
                            if (found) out[sorted_idx[i]] = best;
                            else win_set_null(rvec, sorted_idx[i]);
                        }
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    int64_t best = want_max ? INT64_MIN : INT64_MAX;
                    int found = 0;
                    for (int64_t i = ps; i < pe; i++) {
                        int64_t row = sorted_idx[i];
                        if (ray_vec_is_null(fvec, row)) continue;
                        int64_t v = win_read_i64(fvec, row);
                        if (!found || (want_max ? v > best : v < best)) { best = v; found = 1; }
                    }
                    for (int64_t i = ps; i < pe; i++) {
                        if (found) out[sorted_idx[i]] = best;
                        else win_set_null(rvec, sorted_idx[i]);
                    }
                } else {
                    if (!trailing) {
                        int64_t best = want_max ? INT64_MIN : INT64_MAX;
                        int found = 0;
                        for (int64_t i = ps; i < pe; i++) {
                            int64_t row = sorted_idx[i];
                            if (!ray_vec_is_null(fvec, row)) {
                                int64_t v = win_read_i64(fvec, row);
                                if (!found || (want_max ? v > best : v < best)) { best = v; found = 1; }
                            }
                            if (found) out[row] = best;
                            else win_set_null(rvec, row);
                        }
                    } else {
                        for (int64_t i = ps; i < pe; i++) {
                            int64_t lo = i - frame_start_n;
                            if (lo < ps) lo = ps;
                            int64_t best = want_max ? INT64_MIN : INT64_MAX;
                            int found = 0;
                            for (int64_t j = lo; j <= i; j++) {
                                int64_t row = sorted_idx[j];
                                if (ray_vec_is_null(fvec, row)) continue;
                                int64_t v = win_read_i64(fvec, row);
                                if (!found || (want_max ? v > best : v < best)) { best = v; found = 1; }
                            }
                            if (found) out[sorted_idx[i]] = best;
                            else win_set_null(rvec, sorted_idx[i]);
                        }
                    }
                }
            }
            break;
        }
        case RAY_WIN_LAG: {
            if (!fvec) break;
            int64_t offset = func_params[f];
            if (offset <= 0) offset = 1;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i - offset;
                    if (src >= ps) {
                        out[sorted_idx[i]] = win_read_f64(fvec, sorted_idx[src]);
                        if (ray_vec_is_null(fvec, sorted_idx[src]))
                            win_set_null(rvec, sorted_idx[i]);
                    } else {
                        out[sorted_idx[i]] = 0.0;
                        win_set_null(rvec, sorted_idx[i]);
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i - offset;
                    if (src >= ps) {
                        out[sorted_idx[i]] = win_read_i64(fvec, sorted_idx[src]);
                        if (ray_vec_is_null(fvec, sorted_idx[src]))
                            win_set_null(rvec, sorted_idx[i]);
                    } else {
                        out[sorted_idx[i]] = 0;
                        win_set_null(rvec, sorted_idx[i]);
                    }
                }
            }
            break;
        }
        case RAY_WIN_LEAD: {
            if (!fvec) break;
            int64_t offset = func_params[f];
            if (offset <= 0) offset = 1;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i + offset;
                    if (src < pe) {
                        out[sorted_idx[i]] = win_read_f64(fvec, sorted_idx[src]);
                        if (ray_vec_is_null(fvec, sorted_idx[src]))
                            win_set_null(rvec, sorted_idx[i]);
                    } else {
                        out[sorted_idx[i]] = 0.0;
                        win_set_null(rvec, sorted_idx[i]);
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = i + offset;
                    if (src < pe) {
                        out[sorted_idx[i]] = win_read_i64(fvec, sorted_idx[src]);
                        if (ray_vec_is_null(fvec, sorted_idx[src]))
                            win_set_null(rvec, sorted_idx[i]);
                    } else {
                        out[sorted_idx[i]] = 0;
                        win_set_null(rvec, sorted_idx[i]);
                    }
                }
            }
            break;
        }
        case RAY_WIN_FIRST_VALUE: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = trailing ? i - frame_start_n : ps;
                    if (src < ps) src = ps;
                    bool is_null = ray_vec_is_null(fvec, sorted_idx[src]);
                    out[sorted_idx[i]] = is_null ? 0.0 : win_read_f64(fvec, sorted_idx[src]);
                    if (is_null) win_set_null(rvec, sorted_idx[i]);
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t src = trailing ? i - frame_start_n : ps;
                    if (src < ps) src = ps;
                    bool is_null = ray_vec_is_null(fvec, sorted_idx[src]);
                    out[sorted_idx[i]] = is_null ? 0 : win_read_i64(fvec, sorted_idx[src]);
                    if (is_null) win_set_null(rvec, sorted_idx[i]);
                }
            }
            break;
        }
        case RAY_WIN_LAST_VALUE: {
            if (!fvec) break;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                if (whole) {
                    bool lnull = ray_vec_is_null(fvec, sorted_idx[pe - 1]);
                    double last = lnull ? 0.0 : win_read_f64(fvec, sorted_idx[pe - 1]);
                    for (int64_t i = ps; i < pe; i++) {
                        out[sorted_idx[i]] = last;
                        if (lnull) win_set_null(rvec, sorted_idx[i]);
                    }
                } else {
                    for (int64_t i = ps; i < pe; i++) {
                        out[sorted_idx[i]] = win_read_f64(fvec, sorted_idx[i]);
                        if (ray_vec_is_null(fvec, sorted_idx[i]))
                            win_set_null(rvec, sorted_idx[i]);
                    }
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                if (whole) {
                    bool lnull = ray_vec_is_null(fvec, sorted_idx[pe - 1]);
                    int64_t last = lnull ? 0 : win_read_i64(fvec, sorted_idx[pe - 1]);
                    for (int64_t i = ps; i < pe; i++) {
                        out[sorted_idx[i]] = last;
                        if (lnull) win_set_null(rvec, sorted_idx[i]);
                    }
                } else {
                    for (int64_t i = ps; i < pe; i++) {
                        out[sorted_idx[i]] = win_read_i64(fvec, sorted_idx[i]);
                        if (ray_vec_is_null(fvec, sorted_idx[i]))
                            win_set_null(rvec, sorted_idx[i]);
                    }
                }
            }
            break;
        }
        case RAY_WIN_NTH_VALUE: {
            if (!fvec) break;
            int64_t nth = func_params[f];
            if (nth < 1) nth = 1;
            if (is_f64[f]) {
                double* out = (double*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t lo = trailing ? i - frame_start_n : ps;
                    if (lo < ps) lo = ps;
                    int64_t src = lo + nth - 1;
                    bool is_null = src > (whole ? pe - 1 : i) ||
                                   ray_vec_is_null(fvec, sorted_idx[src]);
                    out[sorted_idx[i]] = is_null ? 0.0 : win_read_f64(fvec, sorted_idx[src]);
                    if (is_null) win_set_null(rvec, sorted_idx[i]);
                }
            } else {
                int64_t* out = (int64_t*)ray_data(rvec);
                for (int64_t i = ps; i < pe; i++) {
                    int64_t lo = trailing ? i - frame_start_n : ps;
                    if (lo < ps) lo = ps;
                    int64_t src = lo + nth - 1;
                    bool is_null = src > (whole ? pe - 1 : i) ||
                                   ray_vec_is_null(fvec, sorted_idx[src]);
                    out[sorted_idx[i]] = is_null ? 0 : win_read_i64(fvec, sorted_idx[src]);
                    if (is_null) win_set_null(rvec, sorted_idx[i]);
                }
            }
            break;
        }
        } /* switch */
    } /* for each func */
}

/* Parallel per-partition window compute context */
typedef struct {
    ray_t** order_vecs;
    uint32_t n_order;
    ray_t** func_vecs;
    uint8_t* func_kinds;
    int64_t* func_params;
    uint32_t n_funcs;
    uint8_t frame_start;
    uint8_t frame_end;
    int64_t frame_start_n;
    int64_t* sorted_idx;
    int64_t* part_offsets;
    ray_t** result_vecs;
    bool* is_f64;
} win_par_ctx_t;

static void win_par_fn(void* arg, uint32_t worker_id,
                       int64_t start, int64_t end) {
    (void)worker_id;
    win_par_ctx_t* ctx = (win_par_ctx_t*)arg;
    for (int64_t p = start; p < end; p++) {
        win_compute_partition(
            ctx->order_vecs, ctx->n_order,
            ctx->func_vecs, ctx->func_kinds, ctx->func_params,
            ctx->n_funcs, ctx->frame_start, ctx->frame_end, ctx->frame_start_n,
            ctx->sorted_idx, ctx->part_offsets[p], ctx->part_offsets[p + 1],
            ctx->result_vecs, ctx->is_f64);
    }
}

/* Parallel gather of partition key values into contiguous array.
 * Eliminates random-access reads during Pass 2 boundary detection. */
typedef struct {
    const int64_t* sorted_idx;
    uint64_t*      pkey_sorted;
    ray_t**         sort_vecs;
    uint32_t       n_part;
} pkey_gather_ctx_t;

static void pkey_gather_fn(void* arg, uint32_t wid,
                            int64_t start, int64_t end) {
    (void)wid;
    pkey_gather_ctx_t* ctx = (pkey_gather_ctx_t*)arg;
    const int64_t* sidx = ctx->sorted_idx;
    uint64_t* out = ctx->pkey_sorted;

    if (ctx->n_part == 1) {
        ray_t* pk = ctx->sort_vecs[0];
        const void* pkd = ray_data(pk);
        if (RAY_IS_SYM(pk->type)) {
            for (int64_t i = start; i < end; i++)
                out[i] = (uint64_t)ray_read_sym(pkd, sidx[i], pk->type, pk->attrs);
        } else if (pk->type == RAY_I32 || pk->type == RAY_DATE || pk->type == RAY_TIME) {
            const int32_t* src = (const int32_t*)pkd;
            /* Map signed [INT32_MIN..INT32_MAX] to unsigned [0..UINT32_MAX]
             * by flipping the sign bit; computed as unsigned to avoid
             * signed-integer-overflow UB on positive minus INT32_MIN. */
            for (int64_t i = start; i < end; i++)
                out[i] = (uint64_t)((uint32_t)src[sidx[i]] ^ 0x80000000u);
        } else {
            const uint64_t* src = (const uint64_t*)pkd;
            for (int64_t i = start; i < end; i++)
                out[i] = src[sidx[i]];
        }
    } else {
        for (int64_t i = start; i < end; i++) {
            int64_t r = sidx[i];
            uint64_t key = 0;
            for (uint32_t k = 0; k < ctx->n_part; k++) {
                ray_t* col = ctx->sort_vecs[k];
                const void* d = ray_data(col);
                if (RAY_IS_SYM(col->type))
                    key = (key << 32) | (uint32_t)ray_read_sym(d, r, col->type, col->attrs);
                else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME)
                    /* Sign-bit flip: avoids signed-integer-overflow UB. */
                    key = (key << 32) | ((uint32_t)((const int32_t*)d)[r] ^ 0x80000000u);
                else {
                    key = (key << 32) | (uint32_t)((const uint64_t*)d)[r];
                }
            }
            out[i] = key;
        }
    }
}

/* Segment count of a parted table = length of any parted column's segs
 * array (mirrors build_segment_table's own read, exec.c:3377-3384). */
static int64_t window_parted_seg_count(ray_t* tbl) {
    int64_t nc = ray_table_ncols(tbl);
    for (int64_t c = 0; c < nc; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (col && RAY_IS_PARTED(col->type)) return col->len;
    }
    return 0;  /* not parted (a table of only MAPCOMMON keys can't occur) */
}

/* ── streaming case-a parted window (partition-key-aligned) ────────────────
 * Monotonic count of per-segment runs taken by the case-a concat fast-path
 * (PARTITION BY includes the physical/date key).  Bumped once per segment;
 * surfaced via (.sys.mem)'s "window-perpart-runs" (mirrors
 * ray_sort_perpart_runs, query.c). */
static _Atomic(int64_t) ray_window_perpart_runs_ctr = 0;

int64_t ray_window_perpart_runs(void) {
    return atomic_load_explicit(&ray_window_perpart_runs_ctr,
                                 memory_order_relaxed);
}

ray_t* exec_window(ray_graph_t* g, ray_op_t* op, ray_t* tbl) {
    if (!tbl || RAY_IS_ERR(tbl)) return tbl;

    ray_op_ext_t* ext = find_ext(g, op->id);
    if (!ext) return ray_error("nyi", NULL);

    /* Frame-spec validation: the compute path honors the whole partition,
     * the running frame, and fixed trailing ROWS frames.  Other numeric
     * bounds and RANGE-mode CURRENT ROW must be rejected loudly rather than
     * silently degrading to a different frame.
     *   - whole:    UNBOUNDED PRECEDING .. UNBOUNDED FOLLOWING
     *   - running:  UNBOUNDED PRECEDING .. CURRENT ROW in ROWS mode
     *   - trailing: N PRECEDING .. CURRENT ROW in ROWS mode */
    {
        uint8_t fs = ext->window.frame_start;
        uint8_t fe = ext->window.frame_end;
        bool whole_frame   = (fs == RAY_BOUND_UNBOUNDED_PRECEDING &&
                              fe == RAY_BOUND_UNBOUNDED_FOLLOWING);
        bool running_frame = (fs == RAY_BOUND_UNBOUNDED_PRECEDING &&
                              fe == RAY_BOUND_CURRENT_ROW &&
                              ext->window.frame_type == RAY_FRAME_ROWS);
        bool trailing_frame = (fs == RAY_BOUND_N_PRECEDING &&
                               fe == RAY_BOUND_CURRENT_ROW &&
                               ext->window.frame_type == RAY_FRAME_ROWS &&
                               ext->window.frame_start_n >= 0);
        if (!whole_frame && !running_frame && !trailing_frame)
            return ray_error("nyi", "unsupported window frame: only "
                "whole, running, and fixed trailing ROWS frames are honored");
    }

    /* Case-a streaming fast-path: if PARTITION BY includes the physical/date
     * partition key (a RAY_MAPCOMMON column — one constant value per day
     * segment), every logical window partition lives wholly inside one day
     * segment.  Running exec_window per-segment and concatenating in
     * segment order is byte-identical to the flat result (no cross-partition
     * carry, no whole-table sort) — the flat path scatters results to
     * original row positions and passes columns through unpermuted, and
     * flattening a parted table IS the segment concat, so
     * per-segment-compute-then-concat == flat.  Must run BEFORE the Task-1
     * flatten guard below, which is the correct-but-unstreamed fallback for
     * every shape this fast-path declines (no PARTITION BY, or a
     * PARTITION BY that excludes the date key and can span segments). */
    {
        int64_t nseg = window_parted_seg_count(tbl);
        if (nseg >= 1) {
            bool eligible = false;
            for (uint32_t k = 0; k < ext->window.n_part_keys && !eligible; k++) {
                ray_op_ext_t* key_ext = find_ext(g, ext->window.part_keys[k]);
                if (key_ext && key_ext->base.opcode == OP_SCAN) {
                    ray_t* col = ray_table_get_col(tbl, key_ext->sym);
                    if (col && col->type == RAY_MAPCOMMON) eligible = true;
                }
            }
            if (eligible) {
                ray_t* merged = NULL;
                for (int64_t s = 0; s < nseg; s++) {
                    ray_t* seg = build_segment_table(tbl, (int32_t)s);
                    if (!seg || RAY_IS_ERR(seg)) {
                        ray_release(merged);
                        return seg ? seg : ray_error("oom", NULL);
                    }
                    /* seg is flat (no PARTED/MAPCOMMON column) so this
                     * recursion re-checks eligibility as false and falls
                     * through to the ordinary flat compute — terminates. */
                    ray_t* rseg = exec_window(g, op, seg);
                    if (!rseg || RAY_IS_ERR(rseg)) {
                        ray_release(seg);
                        ray_release(merged);
                        return rseg ? rseg : ray_error("oom", NULL);
                    }
                    ray_release(seg);
                    atomic_fetch_add_explicit(&ray_window_perpart_runs_ctr, 1,
                                               memory_order_relaxed);
                    if (!merged) {
                        merged = rseg;
                    } else {
                        ray_t* m = ray_result_merge(merged, rseg);
                        ray_release(merged);
                        ray_release(rseg);
                        if (!m || RAY_IS_ERR(m)) return m ? m : ray_error("oom", NULL);
                        merged = m;
                    }
                }
                return merged;
            }
        }
    }

    /* Parted-input correctness guard: win_resolve_vec's OP_SCAN shortcut
     * (above) returns raw parted/MAPCOMMON columns, which every compute
     * loop below silently no-ops on (degenerate 0/NULL window values).
     * Flatten (concat all segments) before compute — correct for every
     * window shape, since the flat path scatters results to original
     * row positions and passes through unpermuted columns. */
    {
        int64_t nc = ray_table_ncols(tbl);
        for (int64_t c = 0; c < nc; c++) {
            ray_t* col = ray_table_get_col_idx(tbl, c);
            if (col && (RAY_IS_PARTED(col->type) || col->type == RAY_MAPCOMMON)) {
                int64_t nseg = window_parted_seg_count(tbl);
                ray_t* flat = NULL;
                for (int64_t s = 0; s < nseg; s++) {
                    ray_t* seg = build_segment_table(tbl, (int32_t)s);
                    if (!seg || RAY_IS_ERR(seg)) {
                        ray_release(flat);
                        return seg ? seg : ray_error("oom", NULL);
                    }
                    if (!flat) {
                        flat = seg;
                    } else {
                        ray_t* m = ray_result_merge(flat, seg);
                        ray_release(flat);
                        ray_release(seg);
                        if (!m || RAY_IS_ERR(m)) return m ? m : ray_error("oom", NULL);
                        flat = m;
                    }
                }
                /* flat has no parted columns: recurses exactly once. */
                ray_t* r = exec_window(g, op, flat);
                ray_release(flat);
                return r;
            }
        }
    }

    int64_t nrows = ray_table_nrows(tbl);
    int64_t ncols = ray_table_ncols(tbl);
    uint32_t n_part  = ext->window.n_part_keys;
    uint32_t n_order = ext->window.n_order_keys;
    uint32_t n_funcs = ext->window.n_funcs;
    uint32_t n_sort  = n_part + n_order;

    if (nrows == 0 || n_funcs == 0) {
        ray_retain(tbl);
        return tbl;
    }

    /* --- Phase 0: Resolve key and func_input vectors --- */
    /* Frame-entry scratch: one carve for every array whose lifetime spans
     * the whole function (freed at every exit incl. `oom:`) — sort_vecs/
     * sort_owned/sort_descs, func_vecs/func_owned/result_vecs/is_f64, and
     * win_enum_rank_hdrs (Pass 1's per-key rank-map headers, hoisted here
     * since it shares this lifetime).  n_sort/n_funcs are unbounded
     * post-migration, so this can no longer be stack VLAs.  Layout:
     * 8-byte pointer arrays first, then the 1-byte flag arrays.
     * scratch_calloc zero-inits the whole block: result_vecs and (in the
     * radix-block carve below) enum_ranks are read across their FULL
     * range on some exits before every slot has necessarily been written
     * (e.g. a mid-loop OOM leaves later funcs' result_vecs untouched), so
     * those slots must read back NULL — calloc'ing the whole carve is
     * simpler and cheaper-than-hot-path-relevant than splitting it into
     * a calloc'd sub-range and an alloc'd one. */
    uint32_t n_sort_alloc = n_sort > 0 ? n_sort : 1;
    size_t frame_ptr_bytes  = (size_t)n_sort_alloc * sizeof(ray_t*) * 2   /* sort_vecs, win_enum_rank_hdrs */
                            + (size_t)n_funcs      * sizeof(ray_t*) * 2; /* func_vecs, result_vecs */
    size_t frame_flag_bytes = (size_t)n_sort_alloc * 2                   /* sort_owned, sort_descs */
                            + (size_t)n_funcs      * 2;                  /* func_owned, is_f64 */
    ray_t* frame_hdr;
    char* frame_base = (char*)scratch_calloc(&frame_hdr,
                                              frame_ptr_bytes + frame_flag_bytes);
    if (!frame_base) return ray_error("oom", NULL);
    ray_t** sort_vecs          = (ray_t**)frame_base;
    ray_t** func_vecs          = sort_vecs + n_sort_alloc;
    ray_t** result_vecs        = func_vecs + n_funcs;
    ray_t** win_enum_rank_hdrs = result_vecs + n_funcs;
    uint8_t* sort_owned = (uint8_t*)(win_enum_rank_hdrs + n_sort_alloc);
    uint8_t* sort_descs = sort_owned + n_sort_alloc;
    uint8_t* func_owned = sort_descs + n_sort_alloc;
    bool*    is_f64     = (bool*)(func_owned + n_funcs);

    for (uint32_t k = 0; k < n_part; k++) {
        sort_vecs[k] = win_resolve_vec(g, op_node(g, ext->window.part_keys[k]), tbl,
                                        &sort_owned[k]);
        sort_descs[k] = 0;  /* partition keys always ASC */
        if (!sort_vecs[k] || RAY_IS_ERR(sort_vecs[k])) {
            ray_t* err = sort_vecs[k] ? sort_vecs[k] : ray_error("nyi", NULL);
            for (uint32_t j = 0; j < k; j++)
                if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                    ray_release(sort_vecs[j]);
            scratch_free(frame_hdr);
            return err;
        }
    }
    for (uint32_t k = 0; k < n_order; k++) {
        sort_vecs[n_part + k] = win_resolve_vec(g, op_node(g, ext->window.order_keys[k]),
                                                 tbl, &sort_owned[n_part + k]);
        sort_descs[n_part + k] = ext->window.order_descs[k];
        if (!sort_vecs[n_part + k] || RAY_IS_ERR(sort_vecs[n_part + k])) {
            ray_t* err = sort_vecs[n_part + k] ? sort_vecs[n_part + k]
                                               : ray_error("nyi", NULL);
            for (uint32_t j = 0; j < n_part + k; j++)
                if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                    ray_release(sort_vecs[j]);
            scratch_free(frame_hdr);
            return err;
        }
    }

    for (uint32_t f = 0; f < n_funcs; f++) {
        ray_op_t* fi = op_node(g, ext->window.func_inputs[f]);
        if (fi) {
            func_vecs[f] = win_resolve_vec(g, fi, tbl, &func_owned[f]);
            if (!func_vecs[f] || RAY_IS_ERR(func_vecs[f])) {
                ray_t* err = func_vecs[f] ? func_vecs[f] : ray_error("nyi", NULL);
                for (uint32_t j = 0; j < f; j++)
                    if (func_owned[j] && func_vecs[j] && !RAY_IS_ERR(func_vecs[j]))
                        ray_release(func_vecs[j]);
                for (uint32_t j = 0; j < n_sort; j++)
                    if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                        ray_release(sort_vecs[j]);
                scratch_free(frame_hdr);
                return err;
            }
        } else {
            func_vecs[f] = NULL;
        }
    }

    /* Sequential runtime-id LUT warm-up: win_read_i64/f64 re-express
     * FILE-domain SYM cells via sym_cell_runtime_id inside the win_par_fn
     * workers — the FIRST LUT request interns the domain's vocabulary,
     * which must happen HERE, before any dispatch, never in a worker
     * (sym.c's frozen-table rule).  No-op for runtime-domain columns.
     * LUT-build OOM is a loud error (7a-review hardening): silent -1
     * translations would corrupt window outputs downstream. */
    for (uint32_t f = 0; f < n_funcs; f++) {
        if (!func_vecs[f] || func_vecs[f]->type != RAY_SYM) continue;
        struct ray_sym_domain_s* fdom = ray_sym_vec_domain(func_vecs[f]);
        if (fdom == ray_sym_runtime_domain()) continue;
        if (!ray_sym_domain_runtime_lut(fdom)) {
            ray_t* err = ray_error("oom",
                "window: sym domain runtime-id LUT build failed");
            for (uint32_t j = 0; j < n_funcs; j++)
                if (func_owned[j] && func_vecs[j] && !RAY_IS_ERR(func_vecs[j]))
                    ray_release(func_vecs[j]);
            for (uint32_t j = 0; j < n_sort; j++)
                if (sort_owned[j] && sort_vecs[j] && !RAY_IS_ERR(sort_vecs[j]))
                    ray_release(sort_vecs[j]);
            scratch_free(frame_hdr);
            return err;
        }
    }

    /* --- Pass 1: Sort by (partition_keys ++ order_keys) --- */
    ray_t* radix_itmp_hdr = NULL;

    ray_t* indices_hdr = NULL;
    int64_t* indices = (int64_t*)scratch_alloc(&indices_hdr,
                                (size_t)nrows * sizeof(int64_t));
    if (!indices) goto oom;
    for (int64_t i = 0; i < nrows; i++) indices[i] = i;

    int64_t* sorted_idx = indices;

    if (n_sort > 0 && nrows <= 64) {
        sort_cmp_ctx_t cmp_ctx = {
            .vecs = sort_vecs, .desc = sort_descs,
            .nulls_first = NULL, .n_sort = n_sort,
        };
        sort_insertion(&cmp_ctx, indices, nrows);
    } else if (n_sort > 0) {
        /* --- Radix sort fast path --- */
        bool can_radix = true;
        for (uint32_t k = 0; k < n_sort; k++) {
            if (!sort_vecs[k]) { can_radix = false; break; }
            int8_t t = sort_vecs[k]->type;
            if (t != RAY_I64 && t != RAY_F64 && t != RAY_I32 && t != RAY_I16 &&
                t != RAY_BOOL && t != RAY_U8 && t != RAY_SYM &&
                t != RAY_DATE && t != RAY_TIME && t != RAY_TIMESTAMP) {
                can_radix = false; break;
            }
        }
        bool radix_done = false;

        if (can_radix) {
            ray_pool_t* pool = ray_pool_get();

            /* Second consolidated carve: enum_ranks (SYM rank maps),
             * mins/maxs (multi-key range prescan, filled below).  All
             * three scale with n_sort with no <=16 cap — unlike
             * bit_shifts further down, which stays inside that gate.
             * Scoped to this `if (can_radix)` block and freed at its
             * close, whichever of the n_sort==1 / n_sort>1 arms below
             * runs.  A heap failure here just degrades to the merge-sort
             * fallback (same as any other can_radix=false case) rather
             * than failing the query. */
            ray_t* radix_arrs_hdr = NULL;
            uint32_t** enum_ranks = NULL;
            int64_t* mins = NULL;
            int64_t* maxs = NULL;
            char* radix_arrs_base = (char*)scratch_calloc(&radix_arrs_hdr,
                (size_t)n_sort * (sizeof(uint32_t*) + 2 * sizeof(int64_t)));
            if (!radix_arrs_base) {
                can_radix = false;
            } else {
                enum_ranks = (uint32_t**)radix_arrs_base;
                mins = (int64_t*)(enum_ranks + n_sort);
                maxs = mins + n_sort;

                for (uint32_t k = 0; k < n_sort; k++) {
                    if (RAY_IS_SYM(sort_vecs[k]->type)) {
                        enum_ranks[k] = build_enum_rank(sort_vecs[k], nrows,
                                                         &win_enum_rank_hdrs[k]);
                        if (!enum_ranks[k]) { can_radix = false; break; }
                    }
                }
            }

            if (can_radix && n_sort == 1) {
                /* Single-key sort */
                uint8_t key_nbytes = radix_key_bytes(sort_vecs[0]->type);
                /* Narrow-int + has_nulls uses a +1-shifted encoding —
                 * keep the radix pass aligned with the wider key. */
                if ((sort_vecs[0]->attrs & RAY_ATTR_HAS_NULLS) &&
                    (sort_vecs[0]->type == RAY_BOOL ||
                     sort_vecs[0]->type == RAY_U8 ||
                     sort_vecs[0]->type == RAY_I16) &&
                    key_nbytes < 8) {
                    key_nbytes++;
                }
                ray_pool_t* sk_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool : NULL;
                ray_t *keys_hdr;
                uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
                                    (size_t)nrows * sizeof(uint64_t));
                if (keys) {
                    radix_encode_ctx_t enc = {
                        .keys = keys, .data = ray_data(sort_vecs[0]),
                        .col = sort_vecs[0],
                        .type = sort_vecs[0]->type,
                        .col_attrs = sort_vecs[0]->attrs,
                        .desc = sort_descs[0],
                        .nulls_first = sort_descs[0], /* default: NULLS FIRST for DESC */
                        .enum_rank = enum_ranks[0], .n_keys = 1,
                    };
                    if (sk_pool)
                        ray_pool_dispatch(sk_pool, radix_encode_fn, &enc, nrows);
                    else
                        radix_encode_fn(&enc, 0, 0, nrows);

                    if (nrows <= RADIX_SORT_THRESHOLD) {
                        key_introsort(keys, indices, nrows);
                        sorted_idx = indices;
                        radix_done = true;
                    } else {
                        ray_t *ktmp_hdr, *itmp_hdr;
                        uint64_t* ktmp = (uint64_t*)scratch_alloc(&ktmp_hdr,
                                            (size_t)nrows * sizeof(uint64_t));
                        int64_t*  itmp = (int64_t*)scratch_alloc(&itmp_hdr,
                                            (size_t)nrows * sizeof(int64_t));
                        if (ktmp && itmp) {
                            sorted_idx = radix_sort_run(sk_pool, keys, indices,
                                                         ktmp, itmp, nrows,
                                                         key_nbytes, NULL);
                            radix_done = (sorted_idx != NULL);
                        }
                        scratch_free(ktmp_hdr);
                        if (sorted_idx != itmp) scratch_free(itmp_hdr);
                        else radix_itmp_hdr = itmp_hdr;
                    }
                }
                scratch_free(keys_hdr);
            } else if (can_radix && n_sort > 1) {
                /* Multi-key composite radix sort */
                ray_pool_t* pool2 = pool;
                /* mins/maxs carved above (radix_arrs_base), alongside enum_ranks. */
                /* Wider accumulator: up to 16 keys * 63 bits = 1008,
                 * which would wrap a uint8_t and let an oversized
                 * budget falsely pass the <=64 fits check. */
                uint16_t total_bits = 0;
                bool fits = true;

                ray_pool_t* mk_prescan_pool2 = (nrows >= SMALL_POOL_THRESHOLD) ? pool2 : NULL;
                if (n_sort <= MK_PRESCAN_MAX_KEYS && mk_prescan_pool2) {
                    uint32_t nw = ray_pool_total_workers(mk_prescan_pool2);
                    size_t pw_count = (size_t)nw * n_sort;
                    int64_t pw_mins_stack[512], pw_maxs_stack[512];
                    ray_t *pw_mins_hdr = NULL, *pw_maxs_hdr = NULL;
                    int64_t* pw_mins = (pw_count <= 512)
                        ? pw_mins_stack
                        : (int64_t*)scratch_alloc(&pw_mins_hdr, pw_count * sizeof(int64_t));
                    int64_t* pw_maxs = (pw_count <= 512)
                        ? pw_maxs_stack
                        : (int64_t*)scratch_alloc(&pw_maxs_hdr, pw_count * sizeof(int64_t));
                    for (size_t i = 0; i < pw_count; i++) {
                        pw_mins[i] = INT64_MAX;
                        pw_maxs[i] = INT64_MIN;
                    }
                    mk_prescan_ctx_t pctx = {
                        .vecs = sort_vecs, .enum_ranks = enum_ranks,
                        .n_keys = n_sort, .nrows = nrows, .n_workers = nw,
                        .pw_mins = pw_mins, .pw_maxs = pw_maxs,
                    };
                    ray_pool_dispatch(mk_prescan_pool2, mk_prescan_fn, &pctx, nrows);

                    for (uint32_t k = 0; k < n_sort; k++) {
                        int64_t kmin = INT64_MAX, kmax = INT64_MIN;
                        for (uint32_t w = 0; w < nw; w++) {
                            int64_t wmin = pw_mins[w * n_sort + k];
                            int64_t wmax = pw_maxs[w * n_sort + k];
                            if (wmin < kmin) kmin = wmin;
                            if (wmax > kmax) kmax = wmax;
                        }
                        mins[k] = kmin;
                        maxs[k] = kmax;
                        uint64_t range = (uint64_t)(kmax - kmin);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        total_bits += bits;
                    }
                    if (pw_mins_hdr) scratch_free(pw_mins_hdr);
                    if (pw_maxs_hdr) scratch_free(pw_maxs_hdr);
                } else {
                    for (uint32_t k = 0; k < n_sort; k++) {
                        ray_t* col = sort_vecs[k];
                        int64_t kmin = INT64_MAX, kmax = INT64_MIN;
                        if (enum_ranks[k]) {
                            const void* cdata = ray_data(col);
                            int8_t ctype = col->type;
                            uint8_t cattrs = col->attrs;
                            for (int64_t i = 0; i < nrows; i++) {
                                uint32_t raw = (uint32_t)ray_read_sym(cdata, i, ctype, cattrs);
                                int64_t v = (int64_t)enum_ranks[k][raw];
                                if (v < kmin) kmin = v;
                                if (v > kmax) kmax = v;
                            }
                        } else if (col->type == RAY_I64 || col->type == RAY_TIMESTAMP) {
                            const int64_t* d = (const int64_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = d[i];
                                if (d[i] > kmax) kmax = d[i];
                            }
                        } else if (col->type == RAY_I32 || col->type == RAY_DATE || col->type == RAY_TIME) {
                            const int32_t* d = (const int32_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        } else if (col->type == RAY_I16) {
                            const int16_t* d = (const int16_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        } else if (col->type == RAY_BOOL || col->type == RAY_U8) {
                            const uint8_t* d = (const uint8_t*)ray_data(col);
                            for (int64_t i = 0; i < nrows; i++) {
                                if (d[i] < kmin) kmin = (int64_t)d[i];
                                if (d[i] > kmax) kmax = (int64_t)d[i];
                            }
                        }
                        mins[k] = kmin;
                        maxs[k] = kmax;
                        uint64_t range = (uint64_t)(kmax - kmin);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        total_bits += bits;
                    }
                }

                if (total_bits > 64) fits = false;

                /* radix_encode_ctx_t (internal.h) carries fixed 16-key
                 * mins/ranges/bit_shifts/descs/enum_ranks arrays — this
                 * direct-encode path is only safe up to that width.  Wider
                 * composites (any n_sort above 16) fall through to the
                 * merge sort below rather than overrun those arrays. */
                if (fits && n_sort <= 16) {
                    uint8_t bit_shifts[n_sort];
                    uint8_t accum = 0;
                    for (int k = n_sort - 1; k >= 0; k--) {
                        bit_shifts[k] = accum;
                        uint64_t range = (uint64_t)(maxs[k] - mins[k]);
                        uint8_t bits = 1;
                        while (((uint64_t)1 << bits) <= range && bits < 64)
                            bits++;
                        accum += bits;
                    }

                    uint8_t comp_nbytes = (total_bits + 7) / 8;
                    if (comp_nbytes < 1) comp_nbytes = 1;
                    ray_pool_t* mk_pool = (nrows >= SMALL_POOL_THRESHOLD) ? pool2 : NULL;

                    ray_t *keys_hdr;
                    uint64_t* keys = (uint64_t*)scratch_alloc(&keys_hdr,
                                        (size_t)nrows * sizeof(uint64_t));
                    if (keys) {
                        radix_encode_ctx_t enc = {
                            .keys = keys, .n_keys = n_sort, .vecs = sort_vecs,
                        };
                        for (uint32_t k = 0; k < n_sort; k++) {
                            enc.mins[k] = mins[k];
                            enc.ranges[k] = maxs[k] - mins[k];
                            enc.bit_shifts[k] = bit_shifts[k];
                            enc.descs[k] = sort_descs[k];
                            enc.enum_ranks[k] = enum_ranks[k];
                        }
                        if (mk_pool)
                            ray_pool_dispatch(mk_pool, radix_encode_fn, &enc, nrows);
                        else
                            radix_encode_fn(&enc, 0, 0, nrows);

                        if (nrows <= RADIX_SORT_THRESHOLD) {
                            key_introsort(keys, indices, nrows);
                            sorted_idx = indices;
                            radix_done = true;
                        } else {
                            ray_t *ktmp_hdr, *itmp_hdr;
                            uint64_t* ktmp = (uint64_t*)scratch_alloc(&ktmp_hdr,
                                                (size_t)nrows * sizeof(uint64_t));
                            int64_t*  itmp = (int64_t*)scratch_alloc(&itmp_hdr,
                                                (size_t)nrows * sizeof(int64_t));
                            if (ktmp && itmp) {
                                sorted_idx = radix_sort_run(mk_pool, keys, indices,
                                                             ktmp, itmp, nrows,
                                                             comp_nbytes, NULL);
                                radix_done = (sorted_idx != NULL);
                            }
                            scratch_free(ktmp_hdr);
                            if (sorted_idx != itmp) scratch_free(itmp_hdr);
                            else radix_itmp_hdr = itmp_hdr;
                        }
                    }
                    scratch_free(keys_hdr);
                }
            }
            scratch_free(radix_arrs_hdr);
        }

        /* --- Merge sort fallback --- */
        if (!radix_done) {
            sort_cmp_ctx_t cmp_ctx = {
                .vecs = sort_vecs, .desc = sort_descs,
                .nulls_first = NULL, .n_sort = n_sort,
            };
            ray_t* tmp_hdr;
            int64_t* tmp = (int64_t*)scratch_alloc(&tmp_hdr,
                                (size_t)nrows * sizeof(int64_t));
            if (!tmp) { scratch_free(indices_hdr); indices_hdr = NULL; goto oom; }

            ray_pool_t* pool = ray_pool_get();
            uint32_t nw = pool ? ray_pool_total_workers(pool) : 1;
            if (pool && nw > 1 && nrows > 1024) {
                sort_phase1_ctx_t p1ctx = {
                    .cmp_ctx = &cmp_ctx, .indices = indices, .tmp = tmp,
                    .nrows = nrows, .n_chunks = nw,
                };
                ray_pool_dispatch_n(pool, sort_phase1_fn, &p1ctx, nw);

                int64_t chunk_size = (nrows + nw - 1) / nw;
                int64_t run_size = chunk_size;
                int64_t* src = indices;
                int64_t* dst = tmp;
                while (run_size < nrows) {
                    int64_t n_pairs = (nrows + 2 * run_size - 1) / (2 * run_size);
                    sort_merge_ctx_t mctx = {
                        .cmp_ctx = &cmp_ctx, .src = src, .dst = dst,
                        .nrows = nrows, .run_size = run_size,
                    };
                    if (n_pairs > 1)
                        ray_pool_dispatch_n(pool, sort_merge_fn, &mctx,
                                            (uint32_t)n_pairs);
                    else
                        sort_merge_fn(&mctx, 0, 0, n_pairs);
                    int64_t* t = src; src = dst; dst = t;
                    run_size *= 2;
                }
                if (src != indices)
                    memcpy(indices, src, (size_t)nrows * sizeof(int64_t));
            } else {
                sort_merge_recursive(&cmp_ctx, indices, tmp, nrows);
            }
            scratch_free(tmp_hdr);
            sorted_idx = indices;
        }
    }

    /* --- Pass 2: Find partition boundaries --- */
    /* Overallocate part_offsets to worst case (single-pass, no counting pass) */
    ray_t* poff_hdr = NULL;
    int64_t* part_offsets = (int64_t*)scratch_alloc(&poff_hdr,
                                (size_t)(nrows + 1) * sizeof(int64_t));
    if (!part_offsets) { scratch_free(indices_hdr); goto oom; }

    part_offsets[0] = 0;
    int64_t n_parts = 0;

    if (n_part > 0) {
        /* Check if we can pack partition keys into uint64 for fast gather.
         * Multi-key packing shifts each key by 32 bits, so any key requiring
         * >32 bits in a multi-key scenario would be truncated.  Force fallback
         * when any 64-bit key appears alongside other keys. */
        uint8_t pk_bits = 0;
        bool can_pack = true;
        bool has_64bit_key = false;
        for (uint32_t k = 0; k < n_part; k++) {
            int8_t t = sort_vecs[k]->type;
            if (RAY_IS_SYM(t) || t == RAY_I32 || t == RAY_DATE || t == RAY_TIME) pk_bits += 32;
            else if (t == RAY_I64 || t == RAY_SYM || t == RAY_TIMESTAMP ||
                     t == RAY_F64) { pk_bits += 64; has_64bit_key = true; }
            else { can_pack = false; break; }
            if (pk_bits > 64) { can_pack = false; break; }
        }
        /* If multi-key with any 64-bit type, the <<32 packing truncates.
         * Force sequential fallback for correctness. */
        if (can_pack && n_part > 1 && has_64bit_key) can_pack = false;

        ray_t* pkey_hdr = NULL;
        uint64_t* pkey_sorted = can_pack ?
            (uint64_t*)scratch_alloc(&pkey_hdr, (size_t)nrows * sizeof(uint64_t))
            : NULL;

        if (pkey_sorted) {
            /* Parallel gather partition keys into contiguous array */
            pkey_gather_ctx_t gctx = {
                .sorted_idx = sorted_idx, .pkey_sorted = pkey_sorted,
                .sort_vecs = sort_vecs, .n_part = n_part,
            };
            ray_pool_t* gpool = ray_pool_get();
            if (gpool)
                ray_pool_dispatch(gpool, pkey_gather_fn, &gctx, nrows);
            else
                pkey_gather_fn(&gctx, 0, 0, nrows);

            /* Sequential scan on contiguous data (no random access) */
            for (int64_t i = 1; i < nrows; i++)
                if (pkey_sorted[i] != pkey_sorted[i - 1])
                    part_offsets[++n_parts] = i;

            scratch_free(pkey_hdr);
        } else {
            /* Fallback: single-pass random-access comparison */
            for (int64_t i = 1; i < nrows; i++)
                if (win_keys_differ(sort_vecs, n_part,
                                    sorted_idx[i - 1], sorted_idx[i]))
                    part_offsets[++n_parts] = i;
        }
        part_offsets[++n_parts] = nrows;
    } else {
        /* No partition keys: entire table is one partition.
         * Minor memory waste (part_offsets sized for nrows+1) but no
         * correctness issue — only indices 0 and 1 are used. */
        part_offsets[1] = nrows;
        n_parts = 1;
    }

    /* Check cancellation before expensive per-partition compute */
    {
        ray_pool_t* cpool = ray_pool_get();
        if (pool_cancelled(cpool)) {
            scratch_free(poff_hdr);
            scratch_free(indices_hdr);
            if (radix_itmp_hdr) scratch_free(radix_itmp_hdr);
            for (uint32_t k = 0; k < n_sort; k++)
                if (win_enum_rank_hdrs[k]) scratch_free(win_enum_rank_hdrs[k]);
            for (uint32_t k = 0; k < n_sort; k++)
                if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
                    ray_release(sort_vecs[k]);
            for (uint32_t f = 0; f < n_funcs; f++)
                if (func_owned[f] && func_vecs[f] && !RAY_IS_ERR(func_vecs[f]))
                    ray_release(func_vecs[f]);
            scratch_free(frame_hdr);
            return ray_error("cancel", NULL);
        }
    }

    /* --- Pass 3: Allocate result vectors and compute per-partition --- */
    for (uint32_t f = 0; f < n_funcs; f++) {
        uint8_t kind = ext->window.func_kinds[f];
        ray_t* fvec = func_vecs[f];

        bool out_f64 = false;
        if (kind == RAY_WIN_AVG) {
            out_f64 = true;
        } else if (kind == RAY_WIN_SUM || kind == RAY_WIN_MIN ||
                   kind == RAY_WIN_MAX || kind == RAY_WIN_LAG ||
                   kind == RAY_WIN_LEAD || kind == RAY_WIN_FIRST_VALUE ||
                   kind == RAY_WIN_LAST_VALUE || kind == RAY_WIN_NTH_VALUE) {
            out_f64 = fvec && fvec->type == RAY_F64;
        }

        is_f64[f] = out_f64;
        result_vecs[f] = ray_vec_new(out_f64 ? RAY_F64 : RAY_I64, nrows);
        if (!result_vecs[f] || RAY_IS_ERR(result_vecs[f])) {
            for (uint32_t j = 0; j < f; j++) {
                ray_release(result_vecs[j]);
                result_vecs[j] = NULL;
            }
            scratch_free(poff_hdr);
            scratch_free(indices_hdr);
            goto oom;
        }
        result_vecs[f]->len = nrows;
        /* Pre-stamp every slot with the width-correct null sentinel.  The
         * per-partition compute loops below write valid values into
         * "active" slots and call win_set_null on null-producing slots
         * without re-writing the payload — pre-stamping ensures every
         * null slot already holds the correct sentinel. */
        if (is_f64[f]) {
            double* d = (double*)ray_data(result_vecs[f]);
            for (int64_t i = 0; i < nrows; i++) d[i] = NULL_F64;
        } else {
            int64_t* d = (int64_t*)ray_data(result_vecs[f]);
            for (int64_t i = 0; i < nrows; i++) d[i] = NULL_I64;
        }
    }

    /* Order key vectors start at sort_vecs[n_part] */
    ray_t** order_vecs = n_order > 0 ? &sort_vecs[n_part] : NULL;

    {
        ray_pool_t* p3pool = ray_pool_get();
        if (p3pool && n_parts > 1) {
            win_par_ctx_t pctx = {
                .order_vecs = order_vecs, .n_order = n_order,
                .func_vecs = func_vecs, .func_kinds = ext->window.func_kinds,
                .func_params = ext->window.func_params, .n_funcs = n_funcs,
                .frame_start = ext->window.frame_start,
                .frame_end = ext->window.frame_end,
                .frame_start_n = ext->window.frame_start_n,
                .sorted_idx = sorted_idx, .part_offsets = part_offsets,
                .result_vecs = result_vecs, .is_f64 = is_f64,
            };
            ray_pool_dispatch_n(p3pool, win_par_fn, &pctx, (uint32_t)n_parts);
        } else {
            for (int64_t p = 0; p < n_parts; p++) {
                win_compute_partition(
                    order_vecs, n_order,
                    func_vecs, ext->window.func_kinds, ext->window.func_params,
                    n_funcs, ext->window.frame_start, ext->window.frame_end,
                    ext->window.frame_start_n,
                    sorted_idx, part_offsets[p], part_offsets[p + 1],
                    result_vecs, is_f64);
            }
        }

        /* Set RAY_ATTR_HAS_NULLS on vectors that actually received nulls */
        for (uint32_t f = 0; f < n_funcs; f++)
            win_finalize_nulls(result_vecs[f]);
    }

    /* --- Pass 4: Build result table --- */
    ray_t* result = ray_table_new(ncols + n_funcs);
    if (!result || RAY_IS_ERR(result)) {
        for (uint32_t f = 0; f < n_funcs; f++) {
            ray_release(result_vecs[f]);
            result_vecs[f] = NULL;
        }
        scratch_free(poff_hdr);
        scratch_free(indices_hdr);
        goto oom;
    }

    /* Pass-through original columns */
    for (int64_t c = 0; c < ncols; c++) {
        ray_t* col = ray_table_get_col_idx(tbl, c);
        if (!col) continue;
        int64_t name_id = ray_table_col_name(tbl, c);
        ray_retain(col);
        result = ray_table_add_col(result, name_id, col);
        ray_release(col);
    }

    /* Add window result columns with auto-generated names */
    for (uint32_t f = 0; f < n_funcs; f++) {
        char buf[16] = "_w";
        int pos = 2;
        if (f >= 100) buf[pos++] = '0' + (f / 100);
        if (f >= 10)  buf[pos++] = '0' + ((f / 10) % 10);
        buf[pos++] = '0' + (f % 10);
        buf[pos] = '\0';
        int64_t name_id = ray_sym_intern(buf, (size_t)pos);
        result = ray_table_add_col(result, name_id, result_vecs[f]);
        ray_release(result_vecs[f]);
    }

    scratch_free(poff_hdr);
    if (radix_itmp_hdr) scratch_free(radix_itmp_hdr);
    scratch_free(indices_hdr);
    for (uint32_t k = 0; k < n_sort; k++)
        if (win_enum_rank_hdrs[k]) scratch_free(win_enum_rank_hdrs[k]);

    /* Free owned key/func vectors */
    for (uint32_t k = 0; k < n_sort; k++)
        if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
            ray_release(sort_vecs[k]);
    for (uint32_t f = 0; f < n_funcs; f++)
        if (func_owned[f] && func_vecs[f] && !RAY_IS_ERR(func_vecs[f]))
            ray_release(func_vecs[f]);

    /* frame_hdr backs sort_vecs/sort_owned/func_vecs/func_owned/
     * result_vecs/is_f64/win_enum_rank_hdrs — free it LAST, after every
     * loop above that reads through those arrays (cut-1 UAF lesson on
     * this file: free-order matters when one carve backs the very
     * arrays another cleanup step still needs to read). */
    scratch_free(frame_hdr);

    return result;

oom:
    if (radix_itmp_hdr) scratch_free(radix_itmp_hdr);
    for (uint32_t k = 0; k < n_sort; k++)
        if (win_enum_rank_hdrs[k]) scratch_free(win_enum_rank_hdrs[k]);
    for (uint32_t k = 0; k < n_sort; k++)
        if (sort_owned[k] && sort_vecs[k] && !RAY_IS_ERR(sort_vecs[k]))
            ray_release(sort_vecs[k]);
    for (uint32_t f = 0; f < n_funcs; f++) {
        if (func_owned[f] && func_vecs[f] && !RAY_IS_ERR(func_vecs[f]))
            ray_release(func_vecs[f]);
        if (result_vecs[f] && !RAY_IS_ERR(result_vecs[f]))
            ray_release(result_vecs[f]);
    }
    /* Same free-order rule as the success path above. */
    scratch_free(frame_hdr);
    return ray_error("oom", NULL);
}
