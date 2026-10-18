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

/*
 * Shared filter-predicate machinery for the fused operators.
 *
 * This file holds the predicate-shape detector (ray_fused_group_supported),
 * the per-morsel predicate evaluator (fp_eval_cmp / fp_eval_pred) and the
 * predicate compiler (fp_compile_pred / fp_pred_cleanup).  It was split out
 * of fused_group.c when the shape-specific OP_FILTERED_GROUP operator was
 * retired: the remaining consumer is fused_topk.c (the general fused
 * filter + top-K optimization), which uses both the shape probe and the
 * predicate evaluator/compiler.  Declarations live in fused_pred.h and
 * fused_group.h (ray_fused_group_supported).
 */

#include "ops/colbind.h"
#include "vec/vec.h"
#include "ops/fused_group.h"  /* ray_fused_group_supported decl */
#include "ops/fused_pred.h"
#include "table/domain.h"     /* sym-domain resolution */
#include "table/table.h"       /* ray_table_col_meta */
#include "ops/idxop.h"        /* RAY_IDX_CHUNK_ZONE chunk-skip in fp_eval_cmp */
#include "lang/eval.h"        /* ATTR_QUOTED */
#include "store/enc.h"        /* RAY_ENC_VEC: the range-read vector */
#include "store/enccol.h"     /* ray_enc_chunk_stat_t: the directory gate */
#include "store/enceval.h"    /* encoded leaves: ray_enc_eval_cmp */
#include "vec/str.h"          /* ray_str_t_ptr: STR through a view */

#include <assert.h>
#include <limits.h>
#include <string.h>

/* FP_EQ..FP_GE are 0..5 and OP_EQ..OP_GE 25..30 in the same order. */
static inline uint16_t cmp_op_of(fp_op_t op) { return (uint16_t)(OP_EQ + (int)(op - FP_EQ)); }

/* Recognise a 2-character comparison operator: ==, !=, <=, >=.  Returns
 * the FP_* code or -1 on miss. */
static int fp_op_from_2char(const char* op, size_t len) {
    if (len != 2) return -1;
    if (op[1] == '=') {
        if (op[0] == '=') return 0;  /* FP_EQ */
        if (op[0] == '!') return 1;  /* FP_NE */
        if (op[0] == '<') return 3;  /* FP_LE */
        if (op[0] == '>') return 5;  /* FP_GE */
    }
    return -1;
}

/* Recognise a 1-character comparison operator: <, >. */
static int fp_op_from_1char(const char* op, size_t len) {
    if (len != 1) return -1;
    if (op[0] == '<') return 2;  /* FP_LT */
    if (op[0] == '>') return 4;  /* FP_GT */
    return -1;
}

/* Return 1 when an atom of `atom_type` (negative-typed RAY_*) is a
 * legal RHS for a comparison against a column of `col_type`.  The
 * fused per-row compare reads raw bit patterns from the column and
 * compares against an int64-decoded constant, so column ↔ atom must
 * agree on units.  In particular, mixing temporal units (DATE days
 * vs TIMESTAMP nanoseconds vs TIME microseconds) is rejected — let
 * the unfused engine handle the implicit conversion.  Atom types
 * here are the negative-typed (-RAY_*) atom encoding from values. */
static int fp_atom_col_compatible(int8_t atom_type, int8_t col_type) {
    switch (col_type) {
    case RAY_STR: return atom_type == -RAY_STR;
    case RAY_GUID: return atom_type == -RAY_GUID;
    case RAY_SYM:
        /* SYM compares against a symbol-id atom or a string literal
         * (string is intern-resolved to a sym id at compile time). */
        return atom_type == -RAY_SYM || atom_type == -RAY_STR;
    case RAY_DATE:
        return atom_type == -RAY_DATE;
    case RAY_TIME:
        return atom_type == -RAY_TIME;
    case RAY_TIMESTAMP:
        return atom_type == -RAY_TIMESTAMP;
    case RAY_BOOL:
    case RAY_U8:
    case RAY_I16:
    case RAY_I32:
    case RAY_F32: case RAY_F64:
    case RAY_I64:
        /* Any signed/unsigned integer literal; we still range-check
         * cval against the column width to fold out-of-range. */
        return atom_type == -RAY_BOOL || atom_type == -RAY_U8
            || atom_type == -RAY_I16  || atom_type == -RAY_I32
            || atom_type == -RAY_I64 || atom_type == -RAY_F32 || atom_type == -RAY_F64;
    default:
        return 0;
    }
}

/* Planning-time admission by metadata (the bound-source forms are
 * fp_src_supported_op / fp_src_supported below): numeric, temporal, STR and
 * GUID comparisons have an explicit typed leg with null-as-minimum
 * ordering, SYM equality uses domain codes, anything else needs a null-free
 * column from ray_table_col_has_nulls, never a decode. */
static int fp_meta_supported_op(ray_t* tbl, int64_t sym, const ray_col_meta_t* m, int eq_or_ne) {
    if (m->type >= RAY_BOOL && m->type <= RAY_TIMESTAMP) return 1;
    if (m->type == RAY_GUID || m->type == RAY_STR) return 1;
    if (eq_or_ne && m->type == RAY_SYM) return 1;
    return !ray_table_col_has_nulls(tbl, sym);
}

static int fp_expr_const_str(ray_t* expr) {
    if (!expr) return 0;
    if (expr->type == -RAY_STR && !(expr->attrs & ATTR_QUOTED)) return 1;
    if (expr->type != RAY_LIST || ray_len(expr) < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return 0;
    ray_t* head = ray_sym_str(elems[0]->i64);
    if (!head) return 0;
    int is_concat = (ray_str_len(head) == 6
                     && memcmp(ray_str_ptr(head), "concat", 6) == 0);
    if (!is_concat) return 0;
    for (int64_t i = 1; i < ray_len(expr); i++)
        if (!fp_expr_const_str(elems[i])) return 0;
    return 1;
}

/* Is `expr` a phase-3 simple comparison form (op col const)?  Validates
 * that the column exists in `tbl` and that ordering ops only target
 * non-SYM columns.  Returns the FP_* code on success, or -1 on miss. */
static int fp_check_simple_cmp(ray_t* expr, ray_t* tbl) {
    if (!expr || expr->type != RAY_LIST) return -1;
    if (ray_len(expr) != 3) return -1;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return -1;
    ray_t* op_sym = ray_sym_str(elems[0]->i64);
    if (!op_sym) return -1;
    size_t op_len = ray_str_len(op_sym);
    const char* op = ray_str_ptr(op_sym);
    int code = fp_op_from_2char(op, op_len);
    if (code < 0) code = fp_op_from_1char(op, op_len);
    if (code < 0) return -1;

    ray_t* lhs = elems[1];
    if (!lhs || lhs->type != -RAY_SYM || (lhs->attrs & ATTR_QUOTED))
        return -1;
    ray_t* rhs = elems[2];
    if (!rhs || !ray_is_atom(rhs) ||
        (rhs->type == -RAY_SYM && !(rhs->attrs & ATTR_QUOTED)))
        return -1;

    /* Resolve column type to gate ordering ops AND verify the column
     * is fused-supported (no nulls, supported type) AND the constant's
     * atom type is compatible with the column's storage class.  This
     * mirrors fp_compile_cmp exactly so the planner gate and executor
     * compile agree — divergence here means the executor returns
     * `nyi` on shapes the planner thought were ok. */
    if (tbl) {
        ray_col_meta_t m;
        if (!ray_table_col_meta(tbl, lhs->i64, &m)) return -1;
        if (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON) return -1;
        int8_t ct = m.type;
        int is_ord = (code >= 2);  /* LT/LE/GT/GE */
        if (is_ord && ct == RAY_SYM) return -1;
        /* Dict-encoded STR: EQ/NE only, compared on the int32 code vector
         * (resolve the RHS string literal to its dict code at compile —
         * see fp_compile_cmp).  Mirrors the SYM W32 path.  Ordering ops
         * are meaningless on first-occurrence codes, so reject them. */
        int is_dict_str = (ct == RAY_STR && !is_ord
                           && rhs->type == -RAY_STR
                           && m.index_kind == RAY_IDX_DICT);
        /* Only types with a matching typed comparison implementation. */
        if (!is_dict_str
            && ct != RAY_SYM && ct != RAY_BOOL && ct != RAY_U8
            && ct != RAY_I16 && ct != RAY_I32 && ct != RAY_I64
            && ct != RAY_DATE && ct != RAY_TIME && ct != RAY_TIMESTAMP
            && ct != RAY_F32 && ct != RAY_F64 && ct != RAY_STR && ct != RAY_GUID)
            return -1;
        if (!fp_meta_supported_op(tbl, lhs->i64, &m, code == 0 || code == 1)) return -1;
        if (!is_dict_str && !fp_atom_col_compatible(rhs->type, ct)) return -1;
    }
    return code;
}

static int fp_check_like(ray_t* expr, ray_t* tbl) {
    if (!expr || expr->type != RAY_LIST) return 0;
    if (ray_len(expr) != 3) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return 0;
    ray_t* op_sym = ray_sym_str(elems[0]->i64);
    if (!op_sym || ray_str_len(op_sym) != 4
        || memcmp(ray_str_ptr(op_sym), "like", 4) != 0)
        return 0;
    ray_t* lhs = elems[1];
    if (!lhs || lhs->type != -RAY_SYM || (lhs->attrs & ATTR_QUOTED))
        return 0;
    if (!fp_expr_const_str(elems[2])) return 0;
    if (tbl) {
        ray_col_meta_t m;
        /* A null text cell is the empty string on every like path (the
         * kernel and this evaluator both match the pattern against ""),
         * so a nullable column is admitted. */
        if (!ray_table_col_meta(tbl, lhs->i64, &m)) return 0;
        if (m.type != RAY_STR && m.type != RAY_SYM) return 0;
    }
    return 1;
}

static int fp_int_family(int8_t t) {
    return t == RAY_BOOL || t == RAY_U8 || t == RAY_I16 || t == RAY_I32 ||
           t == RAY_I64 || t == RAY_DATE || t == RAY_TIME ||
           t == RAY_TIMESTAMP;
}

static int fp_check_in(ray_t* expr, ray_t* tbl) {
    if (!expr || expr->type != RAY_LIST) return 0;
    if (ray_len(expr) != 3) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return 0;
    ray_t* op_sym = ray_sym_str(elems[0]->i64);
    if (!op_sym || ray_str_len(op_sym) != 2
        || memcmp(ray_str_ptr(op_sym), "in", 2) != 0)
        return 0;
    ray_t* lhs = elems[1];
    ray_t* rhs = elems[2];
    if (!lhs || lhs->type != -RAY_SYM || (lhs->attrs & ATTR_QUOTED))
        return 0;
    if (!rhs || !ray_is_vec(rhs) || (rhs->attrs & RAY_ATTR_SORTED))
        return 0;
    if (ray_len(rhs) > 16) return 0;
    if (!fp_int_family(rhs->type)) return 0;
    if (tbl) {
        ray_col_meta_t m;
        if (!ray_table_col_meta(tbl, lhs->i64, &m) || ray_table_col_has_nulls(tbl, lhs->i64)) return 0;
        if (RAY_IS_PARTED(m.type) || m.type == RAY_MAPCOMMON) return 0;
        if (!fp_int_family(m.type)) return 0;
    }
    return 1;
}

/* Phase-3 supported shapes:
 *
 *   pred  = simple_cmp | (and simple_cmp simple_cmp …)
 *   simple_cmp = (op col const) where op ∈ {==, !=, <, <=, >, >=}
 *   ordering ops require col to be a numeric/temporal (non-SYM) column.
 *   AND fan-in is bounded by FP_PRED_MAX_CHILDREN (defined in fused_pred.h). */

int ray_fused_group_supported(ray_t* expr, ray_t* tbl) {
    if (!expr || expr->type != RAY_LIST) return 0;
    int64_t n = ray_len(expr);
    if (n < 2) return 0;
    ray_t** elems = (ray_t**)ray_data(expr);
    if (!elems[0] || elems[0]->type != -RAY_SYM) return 0;

    /* Detect (and …). */
    ray_t* head_sym = ray_sym_str(elems[0]->i64);
    if (head_sym) {
        size_t hlen = ray_str_len(head_sym);
        const char* hstr = ray_str_ptr(head_sym);
        if (hlen == 3 && memcmp(hstr, "and", 3) == 0) {
            int64_t k = n - 1;
            if (k < 1 || k > FP_PRED_MAX_CHILDREN) return 0;
            for (int64_t i = 0; i < k; i++) {
                if (fp_check_simple_cmp(elems[i + 1], tbl) < 0
                    && !fp_check_like(elems[i + 1], tbl)
                    && !fp_check_in(elems[i + 1], tbl)) return 0;
            }
            return 1;
        }
    }
    /* Fall through: single simple cmp. */
    return (fp_check_simple_cmp(expr, tbl) >= 0 ||
            fp_check_like(expr, tbl) || fp_check_in(expr, tbl)) ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Per-morsel predicate evaluator
 *
 * Pass 1 only handles a single comparison `(== col const)` / `(!= col const)`
 * against an SYM or numeric column.  The compiled state is built once at
 * exec entry (column resolution + constant decode) and reused for every
 * morsel.  fp_eval_cmp writes 0/1 into bits[0..n) for the corresponding
 * row range; OP_NE flips the result via XOR.
 * ──────────────────────────────────────────────────────────────────────── */

/* fp_op_t / fp_cmp_t / fp_pred_t / FP_PRED_MAX_CHILDREN are defined in
 * fused_pred.h so fused_topk.c (and any future fused-* operator) can
 * reuse the predicate evaluator without duplicating the types. */

/* Inner-loop generator: specialise on element type and operator so the
 * compiler can hoist the comparison and autovectorise the byte-store. */
#define FP_RUN(T, OP) do {                                                  \
    const T* d = (const T*)p->col_base + start;                              \
    T cv = (T)cval;                                                          \
    for (int64_t r = 0; r < n; r++) bits[r] = (uint8_t)(d[r] OP cv);         \
} while (0)

/* Fill bits[0..end-start) with 0/1 based on `cmp` over rows [start, end).
 * Per-(esz, op) specialisation; SYM supports only EQ/NE. */
void fp_eval_cmp(const fp_cmp_t* p, int64_t start, int64_t end,
                 uint8_t* bits)
{
    int64_t n = end - start;
    int64_t cval = p->cval;
    fp_op_t op = p->op;
    int8_t  ct = p->col_type;
    uint8_t esz = p->col_esz;

    if (p->typed_compare) {
        for (int64_t r = start; r < end; r++) {
            bool an = p->col_obj ? ray_vec_is_null(p->col_obj, r)
                                 : ray_raw_is_null(ct, p->col_attrs, p->col_base, r);
            bool bn = p->constant_null;
            int cmp;
            if (an || bn) cmp = an == bn ? 0 : an ? -1 : 1;
            else if (ct == RAY_STR) {
                size_t len = 0, clen = ray_str_len(p->literal);
                const char* str;
                if (p->col_obj) str = ray_str_vec_get(p->col_obj, r, &len);
                else {
                    const ray_str_t* sd = &((const ray_str_t*)p->col_base)[r];
                    len = sd->len;
                    str = len ? ray_str_t_ptr(sd, p->col_pool) : "";
                }
                size_t common = len < clen ? len : clen;
                cmp = common ? memcmp(str, ray_str_ptr(p->literal), common) : 0;
                if (!cmp) cmp = (len > clen) - (len < clen);
            } else if (ct == RAY_GUID) {
                cmp = memcmp((const char*)p->col_base + (size_t)r * 16, ray_data(p->literal->obj), 16);
            } else if (p->float_compare) {
                double v = ct == RAY_F32 ? ((const float*)p->col_base)[r]
                    : ct == RAY_F64 ? ((const double*)p->col_base)[r]
                    : (double)read_col_i64(p->col_base, r, ct, p->col_attrs);
                cmp = (v > p->fval) - (v < p->fval);
            } else {
                int64_t v = read_col_i64(p->col_base, r, ct, p->col_attrs);
                cmp = (v > cval) - (v < cval);
            }
            switch (op) {
                case FP_EQ: bits[r - start] = cmp == 0; break;
                case FP_NE: bits[r - start] = cmp != 0; break;
                case FP_LT: bits[r - start] = cmp < 0; break;
                case FP_LE: bits[r - start] = cmp <= 0; break;
                case FP_GT: bits[r - start] = cmp > 0; break;
                case FP_GE: bits[r - start] = cmp >= 0; break;
                default: bits[r - start] = 0; break;
            }
        }
        return;
    }

    /* Compile-time fold: out-of-range constant ⇒ all-true or all-false. */
    if (p->fold) {
        memset(bits, (p->fold == FP_FOLD_TRUE) ? 1 : 0, (size_t)n);
        return;
    }

    /* Chunk-zone fast path: if the column carries per-chunk min/max
     * metadata and [start, end) fits inside a single chunk, decide the
     * whole morsel from chunk extrema without reading a single value.
     * Only integer/temporal comparisons (EQ/NE/LT/LE/GT/GE) — LIKE/IN
     * have their own evaluators below and SYM ordering is rejected at
     * compile time anyway.  Extrema exclude nulls and the null-aware
     * kernels rank a null below every value (null != c, null < c,
     * null <= c are TRUE), so a chunk holding a null is never decided
     * all-pass for EQ/GT/GE nor all-fail for NE/LT/LE from extrema. */
    if (p->col_obj && (p->col_obj->attrs & RAY_ATTR_HAS_INDEX) &&
        p->col_obj->index)
    {
        ray_index_t* ix = ray_index_payload(p->col_obj->index);
        if (ix->kind == RAY_IDX_CHUNK_ZONE &&
            ix->built_for_len == p->col_obj->len &&
            !ix->u.chunk_zone.is_f64 &&
            (op == FP_EQ || op == FP_NE ||
             op == FP_LT || op == FP_LE ||
             op == FP_GT || op == FP_GE))
        {
            uint8_t log2 = ix->u.chunk_zone.chunk_log2;
            int64_t s_ch = start >> log2;
            int64_t e_ch = (end - 1) >> log2;
            if (s_ch == e_ch && (uint32_t)s_ch < ix->u.chunk_zone.n_chunks) {
                const int64_t* mins = (const int64_t*)ray_data(ix->u.chunk_zone.mins);
                const int64_t* maxs = (const int64_t*)ray_data(ix->u.chunk_zone.maxs);
                int64_t cmin = mins[s_ch], cmax = maxs[s_ch];
                if (cmin <= cmax) {       /* skip empty (all-null) chunks */
                    const uint8_t* nb = (const uint8_t*)ray_data(ix->u.chunk_zone.null_bits);
                    bool has_nulls = (nb[s_ch >> 3] >> (s_ch & 7)) & 1u;
                    /* 0=all-fail, 1=all-pass, -1=mixed */
                    int decision = zone_cmp_core(cmin, cmax, true, has_nulls, false, cmp_op_of(op), cval);
                    if (decision >= 0) {
                        memset(bits, (uint8_t)decision, (size_t)n);
                        return;
                    }
                }
            }
        }
    }

    /* SYM low-card fold: const not in dict ⇒ EQ all-zero / NE all-one.
     * Ordering ops are rejected at compile for SYM, so unreachable here. */
    if (ct == RAY_SYM && !p->cval_in_dict) {
        memset(bits, (op == FP_NE) ? 1 : 0, (size_t)n);
        return;
    }

    if (op == FP_LIKE) {
        if (ct == RAY_SYM) {
            uint32_t lut_n = p->like_lut_count;
            uint8_t* lut = p->like_lut;
            const void* base = p->col_base;
            uint8_t esz_l = p->col_esz;
            ray_t** sym_strings = p->like_sym_strings;
            /* sym_strings == NULL ⇒ FILE-domain column: resolve cell
             * positions through the column's domain (sym-domain Phase 2).
             * The LUT is sized by that domain's count at compile. */
            struct ray_sym_domain_s* like_dom =
                sym_strings ? NULL : p->like_dom ? p->like_dom : ray_sym_vec_domain(p->col_obj);
            int use_simple = p->pat_compiled.shape != RAY_GLOB_SHAPE_NONE;
            for (int64_t r = 0; r < n; r++) {
                uint64_t sid = (uint64_t)read_by_esz(base, start + r, esz_l);
                if (sid >= lut_n || !lut) {
                    bits[r] = p->like_empty_match;
                    continue;
                }
                /* The LUT is shared by the workers: relaxed atomics on its
                 * bytes (every writer stores the same answer for a cell,
                 * so a repeated resolve is the only cost of a race). */
                uint8_t state = __atomic_load_n(&lut[sid], __ATOMIC_RELAXED);
                if (!state) {
                    const char* sp = NULL;
                    size_t sl = 0;
                    if (p->like_raw_ok && (int64_t)sid < p->like_raw.count) {
                        sp = ray_sym_domain_raw_str(&p->like_raw, (int64_t)sid, &sl);
                    } else {
                        ray_t* s = sym_strings ? sym_strings[sid]
                                               : ray_sym_domain_str(like_dom, (int64_t)sid);
                        if (s) { sp = ray_str_ptr(s); sl = ray_str_len(s); }
                    }
                    uint8_t match = 0;
                    if (sp) {
                        match = use_simple
                            ? (uint8_t)ray_glob_match_compiled(&p->pat_compiled, sp, sl)
                            : (uint8_t)ray_glob_match(sp, sl, p->pat_str, p->pat_len);
                    }
                    state = (uint8_t)(match ? 2 : 1);
                    __atomic_store_n(&lut[sid], state, __ATOMIC_RELAXED);
                }
                bits[r] = (uint8_t)(state == 2);
            }
            return;
        }
        if (ct != RAY_STR) {
            memset(bits, 0, (size_t)n);
            return;
        }
        int use_simple = p->pat_compiled.shape != RAY_GLOB_SHAPE_NONE;
        for (int64_t r = 0; r < n; r++) {
            size_t sl = 0;
            const char* sp;
            if (p->col_obj) sp = ray_str_vec_get(p->col_obj, start + r, &sl);
            else {
                const ray_str_t* sd = &((const ray_str_t*)p->col_base)[start + r];
                sl = sd->len;
                sp = sl ? ray_str_t_ptr(sd, p->col_pool) : "";
            }
            if (!sp) sp = "";
            bits[r] = use_simple
                    ? (uint8_t)ray_glob_match_compiled(&p->pat_compiled, sp, sl)
                    : (uint8_t)ray_glob_match(sp, sl, p->pat_str, p->pat_len);
        }
        return;
    }

    if (op == FP_IN) {
        if (p->n_cvals == 0) {
            memset(bits, 0, (size_t)n);
            return;
        }
#define FP_RUN_IN(T) do {                                                   \
            const T* d = (const T*)p->col_base + start;                      \
            for (int64_t r = 0; r < n; r++) {                                \
                int64_t v = (int64_t)d[r];                                   \
                uint8_t hit = 0;                                             \
                for (uint8_t j = 0; j < p->n_cvals; j++)                     \
                    hit |= (uint8_t)(v == p->cvals[j]);                      \
                bits[r] = hit;                                               \
            }                                                                \
        } while (0)
        if (esz == 1) {
            FP_RUN_IN(uint8_t);
            return;
        }
        if (esz == 2) {
            FP_RUN_IN(int16_t);
            return;
        }
        if (esz == 4) {
            FP_RUN_IN(int32_t);
            return;
        }
        FP_RUN_IN(int64_t);
        return;
#undef FP_RUN_IN
    }

    if (esz == 1) {
        switch (op) {
        case FP_EQ: FP_RUN(uint8_t, ==); break;
        case FP_NE: FP_RUN(uint8_t, !=); break;
        case FP_LT: FP_RUN(uint8_t, < ); break;
        case FP_LE: FP_RUN(uint8_t, <=); break;
        case FP_GT: FP_RUN(uint8_t, > ); break;
        case FP_GE: FP_RUN(uint8_t, >=); break;
        case FP_LIKE: case FP_IN: memset(bits, 0, (size_t)n); break;
        }
        return;
    }
    if (esz == 2) {
        if (ct == RAY_SYM) {
            switch (op) {
            case FP_EQ: FP_RUN(uint16_t, ==); break;
            case FP_NE: FP_RUN(uint16_t, !=); break;
            case FP_LT: case FP_LE: case FP_GT: case FP_GE:
            case FP_LIKE: case FP_IN: memset(bits, 0, (size_t)n); break;  /* unreachable */
            }
        } else {
            switch (op) {
            case FP_EQ: FP_RUN(int16_t, ==); break;
            case FP_NE: FP_RUN(int16_t, !=); break;
            case FP_LT: FP_RUN(int16_t, < ); break;
            case FP_LE: FP_RUN(int16_t, <=); break;
            case FP_GT: FP_RUN(int16_t, > ); break;
            case FP_GE: FP_RUN(int16_t, >=); break;
            case FP_LIKE: case FP_IN: memset(bits, 0, (size_t)n); break;
            }
        }
        return;
    }
    if (esz == 4) {
        if (ct == RAY_SYM) {
            switch (op) {
            case FP_EQ: FP_RUN(uint32_t, ==); break;
            case FP_NE: FP_RUN(uint32_t, !=); break;
            case FP_LT: case FP_LE: case FP_GT: case FP_GE:
            case FP_LIKE: case FP_IN: memset(bits, 0, (size_t)n); break;  /* unreachable */
            }
        } else {
            switch (op) {
            case FP_EQ: FP_RUN(int32_t, ==); break;
            case FP_NE: FP_RUN(int32_t, !=); break;
            case FP_LT: FP_RUN(int32_t, < ); break;
            case FP_LE: FP_RUN(int32_t, <=); break;
            case FP_GT: FP_RUN(int32_t, > ); break;
            case FP_GE: FP_RUN(int32_t, >=); break;
            case FP_LIKE: case FP_IN: memset(bits, 0, (size_t)n); break;
            }
        }
        return;
    }
    /* Width 8: I64 / TIMESTAMP (or wide SYM, which we reject for ordering). */
    switch (op) {
    case FP_EQ: FP_RUN(int64_t, ==); break;
    case FP_NE: FP_RUN(int64_t, !=); break;
    case FP_LT: FP_RUN(int64_t, < ); break;
    case FP_LE: FP_RUN(int64_t, <=); break;
    case FP_GT: FP_RUN(int64_t, > ); break;
    case FP_GE: FP_RUN(int64_t, >=); break;
    case FP_LIKE: case FP_IN: memset(bits, 0, (size_t)n); break;
    }
}
#undef FP_RUN

static inline int64_t fp_cmp_read_i64_at(const fp_cmp_t* p, int64_t row) {
    const void* base = p->col_base;
    if (p->col_type == RAY_SYM || p->col_type == RAY_BOOL || p->col_type == RAY_U8)
        return read_by_esz(base, row, p->col_esz);
    switch (p->col_esz) {
    case 1:  return (int64_t)((const uint8_t*)base)[row];
    case 2:  return (int64_t)((const int16_t*)base)[row];
    case 4:  return (int64_t)((const int32_t*)base)[row];
    default: return ((const int64_t*)base)[row];
    }
}

static inline uint8_t fp_eval_cmp_one(const fp_cmp_t* p, int64_t row) {
    if (p->fold)
        return (uint8_t)(p->fold == FP_FOLD_TRUE);
    if (p->col_type == RAY_SYM && !p->cval_in_dict)
        return (uint8_t)(p->op == FP_NE);
    if (p->op == FP_LIKE) {
        if (p->col_type == RAY_SYM) {
            uint64_t sid = (uint64_t)read_by_esz(p->col_base, row, p->col_esz);
            if (sid >= p->like_lut_count || !p->like_lut)
                return p->like_empty_match;
            uint8_t state = __atomic_load_n(&p->like_lut[sid], __ATOMIC_RELAXED);
            if (!state) {
                /* NULL sym_strings ⇒ FILE-domain column (see fp_eval_cmp) */
                const char* sp = NULL;
                size_t sl = 0;
                if (p->like_raw_ok && (int64_t)sid < p->like_raw.count) {
                    sp = ray_sym_domain_raw_str(&p->like_raw, (int64_t)sid, &sl);
                } else {
                    ray_t* s = p->like_sym_strings
                        ? p->like_sym_strings[sid]
                        : ray_sym_domain_str(p->like_dom ? p->like_dom : ray_sym_vec_domain(p->col_obj),
                                             (int64_t)sid);
                    if (s) { sp = ray_str_ptr(s); sl = ray_str_len(s); }
                }
                uint8_t match = 0;
                if (sp) {
                    match = (p->pat_compiled.shape != RAY_GLOB_SHAPE_NONE)
                          ? (uint8_t)ray_glob_match_compiled(&p->pat_compiled, sp, sl)
                          : (uint8_t)ray_glob_match(sp, sl, p->pat_str, p->pat_len);
                }
                state = (uint8_t)(match ? 2 : 1);
                __atomic_store_n(&p->like_lut[sid], state, __ATOMIC_RELAXED);
            }
            return (uint8_t)(state == 2);
        }
        if (p->col_type == RAY_STR) {
            size_t sl = 0;
            const char* sp;
            if (p->col_obj) sp = ray_str_vec_get(p->col_obj, row, &sl);
            else {
                const ray_str_t* sd = &((const ray_str_t*)p->col_base)[row];
                sl = sd->len;
                sp = sl ? ray_str_t_ptr(sd, p->col_pool) : "";
            }
            if (!sp) sp = "";
            return (p->pat_compiled.shape != RAY_GLOB_SHAPE_NONE)
                 ? (uint8_t)ray_glob_match_compiled(&p->pat_compiled, sp, sl)
                 : (uint8_t)ray_glob_match(sp, sl, p->pat_str, p->pat_len);
        }
        return 0;
    }

    int64_t v = fp_cmp_read_i64_at(p, row);
    if (p->op == FP_IN) {
        uint8_t hit = 0;
        for (uint8_t j = 0; j < p->n_cvals; j++)
            hit |= (uint8_t)(v == p->cvals[j]);
        return hit;
    }

    switch (p->op) {
    case FP_EQ: return (uint8_t)(v == p->cval);
    case FP_NE: return (uint8_t)(v != p->cval);
    case FP_LT: return (uint8_t)(v <  p->cval);
    case FP_LE: return (uint8_t)(v <= p->cval);
    case FP_GT: return (uint8_t)(v >  p->cval);
    case FP_GE: return (uint8_t)(v >= p->cval);
    case FP_LIKE:
    case FP_IN:
        break;
    }
    return 0;
}

static void fp_eval_cmp_masked(const fp_cmp_t* p, int64_t start, int64_t end,
                               uint8_t* bits)
{
    int64_t n = end - start;
    if (p->op == FP_LIKE) {
        uint8_t tmp[RAY_MORSEL_ELEMS];
        fp_eval_cmp(p, start, end, tmp);
        for (int64_t r = 0; r < n; r++) bits[r] &= tmp[r];
        return;
    }
    for (int64_t r = 0; r < n; r++) {
        if (bits[r] && !fp_eval_cmp_one(p, start + r))
            bits[r] = 0;
    }
}

/* Evaluate a (possibly ANDed) predicate over rows [start, end).  The
 * first child writes directly into bits[]; subsequent children eval into
 * a stack-resident tmp[] buffer and bitwise-AND into bits. */
void fp_eval_pred(const fp_pred_t* p, int64_t start, int64_t end,
                  uint8_t* bits)
{
    int64_t n = end - start;
    for (uint8_t i = 0; i < p->n_children; i++) assert(!p->children[i].enc);
    if (p->n_children == 0) {
        memset(bits, 1, (size_t)n);
        return;
    }
    fp_eval_cmp(&p->children[0], start, end, bits);
    if (p->n_children == 1) return;
    uint8_t use_masked = 0;
    for (uint8_t i = 0; i < p->n_children; i++)
        use_masked |= (uint8_t)(p->children[i].op == FP_IN);
    if (use_masked) {
        for (uint8_t i = 1; i < p->n_children; i++)
            fp_eval_cmp_masked(&p->children[i], start, end, bits);
    } else {
        uint8_t tmp[RAY_MORSEL_ELEMS];
        for (uint8_t i = 1; i < p->n_children; i++) {
            fp_eval_cmp(&p->children[i], start, end, tmp);
            for (int64_t r = 0; r < n; r++) bits[r] &= tmp[r];
        }
    }
}

void fp_scratch_init(fp_scratch_t* s) {
    for (int i = 0; i < FP_PRED_MAX_CHILDREN; i++) ray_col_scratch_init(&s->sc[i]);
}
void fp_scratch_free(fp_scratch_t* s) {
    for (int i = 0; i < FP_PRED_MAX_CHILDREN; i++) ray_col_scratch_free(&s->sc[i]);
}

/* An encoded child whose answer needs no column read: a fold, or a SYM
 * constant absent from the domain (fp_eval_cmp memsets both). */
static bool fp_reads_nothing(const fp_cmp_t* c) {
    return c->fold || (c->col_type == RAY_SYM && !c->cval_in_dict);
}

/* A child answered on its vector's view (store/enceval.h): a plain
 * integer / temporal compare, or SYM equality with a constant of the
 * domain.  A non-typed child has no nulls (fp_src_supported_op), and SYM
 * nulls are in band, so no null mask is applied. */
static bool fp_view_leaf(const fp_cmp_t* c) {
    if (c->typed_compare || c->fold != FP_FOLD_NONE || c->op > FP_GE) return false;
    if (c->col_type == RAY_SYM) return (c->op == FP_EQ || c->op == FP_NE) && c->cval_in_dict;
    return zone_dir_type(c->col_type);
}

/* Rebind an encoded child to the view of [start, start + n) (one vector),
 * shifted so that row `start` is index 0: rows are passed as [0, n).  The
 * directory gate decides a whole range from its chunk's statistics first,
 * then from its vector's descriptor (*decided 0 / 1, nothing read); a
 * materialization another consumer
 * published meanwhile is read in place, never decoded again.  Otherwise a
 * leaf (fp_view_leaf) is evaluated on the vector's view into dst[0, n)
 * (*decided 2, nothing decoded); a vector without a view is read by
 * range. */
static ray_t* fp_bind_view(const fp_cmp_t* c, int64_t start, int64_t n,
                           ray_col_scratch_t* sc, ray_enc_vview_t* vb, uint8_t* dst,
                           fp_cmp_t* out, int* decided) {
    *out = *c;
    *decided = -1;
    if (c->idict) {
        /* the persisted DICT's codes (P6b), in the mapping: no decode, and
         * never a zone decision (codes are membership values, D6) */
        const int32_t* cd = NULL;
        ray_t* e = ray_enccol_idict_codes(c->idict, start, n, &cd);
        if (e) return e;
        out->col_base = cd;                        /* rows are view-relative, as for a range read */
        out->col_obj = NULL;
        return NULL;
    }
    if (!c->typed_compare && zone_dir_type(c->col_type) && c->op <= FP_GE &&
        (start / RAY_ENC_CHUNK) == ((start + n - 1) / RAY_ENC_CHUNK)) {
        ray_enc_chunk_stat_t st;
        if (ray_col_src_chunk_stat(c->src, start / RAY_ENC_CHUNK, &st)) {
            int d = zone_cmp_core(st.min, st.max, st.has_minmax, st.null_count > 0,
                                  (int64_t)st.null_count == st.rows, cmp_op_of(c->op), c->cval);
            if (d >= 0) { *decided = d; return NULL; }
        }
        /* then the vector's descriptor (the window lies in one vector);
         * answer-only, the window is filled, so no consumer guard */
        int64_t mn, mx; uint32_t nn;
        if (start / RAY_ENC_VEC == (start + n - 1) / RAY_ENC_VEC &&
            ray_col_src_vec_stat(c->src, start / RAY_ENC_VEC, &mn, &mx, &nn)) {
            int d = zone_cmp_core(mn, mx, true, nn > 0, false, cmp_op_of(c->op), c->cval);
            if (d >= 0) { *decided = d; return NULL; }
        }
    }
    ray_t* rd = ray_col_src_ready(c->src);
    if (rd && !RAY_IS_PARTED(rd->type)) {
        ray_t* own = (rd->attrs & RAY_ATTR_SLICE) ? rd->slice_parent : rd;
        out->col_attrs = rd->attrs;
        out->col_esz = ray_sym_elem_size(rd->type, rd->attrs);
        out->col_base = (const char*)ray_data(rd) + (size_t)start * out->col_esz;
        out->col_pool = (rd->type == RAY_STR && own->str_pool && !RAY_IS_ERR(own->str_pool))
                      ? (const char*)ray_data(own->str_pool) : NULL;
        out->col_obj = NULL;                       /* rows are view-relative */
        return NULL;
    }
    if (!rd && fp_view_leaf(c)) {
        ray_t* verr = NULL;
        int vr = ray_col_src_vview(c->src, start / RAY_ENC_VEC, vb, &verr);
        if (vr < 0) return verr;
        int64_t off = start % RAY_ENC_VEC;
        /* a bitmap with nulls cannot occur here (a non-typed child is
         * null-free); were it present, the range read answers exactly */
        if (vr > 0 && !vb->nullmap && off + n <= vb->n) {
            uint8_t whole[RAY_ENC_VEC];                /* a window inside the vector */
            uint8_t* o = off == 0 && n == vb->n ? dst : whole;
            if (ray_enc_eval_cmp(vb, cmp_op_of(c->op), c->cval, 0, o)) {
                if (o != dst) memcpy(dst, whole + off, (size_t)n);
                *decided = 2;
                return NULL;
            }
        }
    }
    ray_col_range_t v;
    ray_t* e = ray_col_range(c->src, start, n, sc, &v);
    if (e) return e;
    if (v.n != n) return ray_error("range", "fused predicate: morsel crosses a vector boundary");
    out->col_base = v.data;
    out->col_pool = v.str_pool;
    out->col_obj = NULL;
    out->col_attrs = v.attrs;
    out->col_esz = ray_sym_elem_size(v.type, v.attrs);
    return NULL;
}

ray_t* fp_eval_pred_rs(const fp_pred_t* p, int64_t start, int64_t end,
                       uint8_t* bits, fp_scratch_t* s) {
    int64_t n = end - start;
    assert(n <= RAY_MORSEL_ELEMS);
    bool any_enc = false;
    for (uint8_t i = 0; i < p->n_children; i++) any_enc |= p->children[i].enc;
    if (!any_enc) { fp_eval_pred(p, start, end, bits); return NULL; }
    /* encoded children read one vector per call: split at vector boundaries */
    int64_t cut = ((start / RAY_ENC_VEC) + 1) * RAY_ENC_VEC;
    if (cut < end) {
        ray_t* e = fp_eval_pred_rs(p, start, cut, bits, s);
        return e ? e : fp_eval_pred_rs(p, cut, end, bits + (cut - start), s);
    }
    /* Children AND into bits; once no row of the window passes, the rest
     * would only AND zeros, so they are not evaluated (nor their vectors
     * decoded). */
    uint8_t tmp[RAY_MORSEL_ELEMS];
    for (uint8_t i = 0; i < p->n_children; i++) {
        const fp_cmp_t* c = &p->children[i];
        uint8_t* dst = i == 0 ? bits : tmp;
        if (c->enc && !fp_reads_nothing(c)) {
            fp_cmp_t local; int decided;
            ray_t* e = fp_bind_view(c, start, n, &s->sc[i], &s->vb, dst, &local, &decided);
            if (e) return e;
            if (decided == 0 || decided == 1) memset(dst, decided, (size_t)n);
            else if (decided < 0) fp_eval_cmp(&local, 0, n, dst);   /* 2: written on the view */
        } else if (c->enc) {
            fp_eval_cmp(c, 0, n, dst);            /* reads no column */
        } else {
            fp_eval_cmp(c, start, end, dst);
        }
        uint8_t any = 0;
        if (i == 0) for (int64_t r = 0; r < n; r++) any |= bits[r];
        else        for (int64_t r = 0; r < n; r++) { bits[r] &= tmp[r]; any |= bits[r]; }
        if (!any) break;                          /* the window is decided: all fail */
    }
    return NULL;
}

/* Nullability of a bound source.  Generation 0 keeps the exact payload
 * test; an encoded source answers from its directory null counts (exact),
 * never a decode. */
static int fp_src_has_nulls(const ray_col_src_t* s) {
    if (s->obj->type != RAY_ENCCOL && ray_is_vec(s->obj)) return ray_vec_has_nulls(s->obj);
    return s->may_null;
}

/* Numeric, temporal, STR and GUID comparisons have an explicit typed leg
 * with null-as-minimum ordering. SYM equality uses domain codes. Any other
 * form needs a null-free source; IN keeps that stricter admission
 * (fp_src_supported) because it uses a different evaluator. */
static int fp_src_supported_op(const ray_col_src_t* s, int eq_or_ne) {
    if (s->type >= RAY_BOOL && s->type <= RAY_TIMESTAMP) return 1;
    if (s->type == RAY_GUID || s->type == RAY_STR) return 1;
    if (eq_or_ne && s->type == RAY_SYM) return 1;
    return !fp_src_has_nulls(s);
}

static int fp_src_supported(const ray_col_src_t* s) {
    return !fp_src_has_nulls(s);
}

/* The step that captures a payload pointer asks for the plain column.  Both
 * callers hold a ready source (fp_bind_fields only when out->enc is false;
 * the STR dictionary leg only for a generation-0 column), so it never decodes. */
static int fp_src_plain(ray_col_src_t* s, ray_t** col, ray_t** err) {
    ray_t* e = ray_col_src_plain(s, col);   /* ready-only */
    if (e) { *err = e; return -1; }
    return 0;
}

/* The column fields of a compiled child: today's pointers for a ready
 * source; the source's metadata and no pointer for an encoded one (a view
 * is bound per morsel by fp_bind_view). */
static int fp_bind_fields(ray_col_src_t* src, fp_cmp_t* out, ray_t** err) {
    if (out->enc) {
        out->col_type  = src->type;
        out->col_attrs = src->attrs;
        out->col_esz   = ray_sym_elem_size(src->type, src->attrs);
        out->col_base  = NULL;
        out->col_obj   = NULL;
        out->col_len   = src->len;
        return 0;
    }
    ray_t* col = NULL;
    if (fp_src_plain(src, &col, err) != 0) return -1;     /* ready-only */
    out->col_type  = col->type;
    out->col_attrs = col->attrs;
    out->col_esz   = ray_sym_elem_size(col->type, col->attrs);
    out->col_base  = ray_data(col);
    out->col_obj   = col;
    out->col_len   = col->len;
    ray_t* own = (col->attrs & RAY_ATTR_SLICE) ? col->slice_parent : col;
    out->col_pool  = (col->type == RAY_STR && own->str_pool && !RAY_IS_ERR(own->str_pool))
                   ? (const char*)ray_data(own->str_pool) : NULL;
    return 0;
}

/* Compile predicate DAG node (a comparison with OP_SCAN lhs and OP_CONST
 * rhs) against `tbl`.  Returns 0 on success, -1 if the shape can't be
 * handled (caller should bail and let the unfused path run). */
static int fp_compile_cmp(ray_graph_t* g, ray_op_t* pred_op, ray_t* tbl,
                          fp_cmp_t* out, ray_t** err)
{
    if (!pred_op || pred_op->arity != 2) return -1;
    out->fold = FP_FOLD_NONE;
    switch (pred_op->opcode) {
    case OP_EQ: out->op = FP_EQ; break;
    case OP_NE: out->op = FP_NE; break;
    case OP_LT: out->op = FP_LT; break;
    case OP_LE: out->op = FP_LE; break;
    case OP_GT: out->op = FP_GT; break;
    case OP_GE: out->op = FP_GE; break;
    case OP_LIKE: out->op = FP_LIKE; break;
    case OP_IN: out->op = FP_IN; break;
    default: return -1;
    }

    ray_op_t* lhs = op_child(g, pred_op, 0);
    ray_op_t* rhs = op_child(g, pred_op, 1);
    if (!lhs || !rhs) return -1;
    if (lhs->opcode != OP_SCAN || rhs->opcode != OP_CONST) return -1;

    ray_op_ext_t* lext = find_ext(g, lhs->id);
    ray_op_ext_t* rext = find_ext(g, rhs->id);
    if (!lext || !rext || !rext->literal) return -1;

    /* Bind without decoding: every decision below reads the bound source;
     * the plain column is requested only where a payload pointer is
     * captured (R-READ: a decline takes the unfused scan path). */
    ray_t* berr = NULL;
    if (ray_col_bind_declines(tbl, lext->sym)) return -1;
    ray_col_src_t* src = ray_graph_bind_col(g, tbl, lext->sym, &berr);
    if (berr) { *err = berr; return -1; }
    if (!src) return -1;
    if (src->parted) return -1;
    out->src = src;
    out->enc = ray_col_src_ready(src) == NULL;     /* a generation-2 single-file column */
    out->col_pool = NULL;
    if (out->op == FP_IN) {
        if (!fp_src_supported(src) || !fp_int_family(src->type)) return -1;
        ray_t* sv = rext->literal;
        if (!sv || !ray_is_vec(sv) || !fp_int_family(sv->type)) return -1;
        if (ray_len(sv) > 16) return -1;
        if (fp_bind_fields(src, out, err) != 0) return -1;
        int8_t st = sv->type;
        int64_t nsv = ray_len(sv);
        int64_t out_n = 0;
        for (int64_t i = 0; i < nsv; i++) {
            if (ray_vec_may_have_nulls(sv) && ray_vec_is_null(sv, i))
                continue;
            switch (st) {
            case RAY_BOOL:
            case RAY_U8:        out->cvals[out_n++] = ((uint8_t*)ray_data(sv))[i]; break;
            case RAY_I16:       out->cvals[out_n++] = ((int16_t*)ray_data(sv))[i]; break;
            case RAY_I32:
            case RAY_DATE:
            case RAY_TIME:      out->cvals[out_n++] = ((int32_t*)ray_data(sv))[i]; break;
            case RAY_I64:
            case RAY_TIMESTAMP: out->cvals[out_n++] = ((int64_t*)ray_data(sv))[i]; break;
            default: return -1;
            }
        }
        out->n_cvals = (uint8_t)out_n;
        out->cval_in_dict = 1;
        return 0;
    }
    if (out->op == FP_LIKE) {
        /* A nullable text column is admitted: a null cell is the empty
         * string on every like path, matched against the pattern once
         * here (like_empty_match). */
        if (src->type != RAY_STR && src->type != RAY_SYM) return -1;
        ray_t* cv_like = rext->literal;
        if (!cv_like || cv_like->type != -RAY_STR) return -1;
        if (fp_bind_fields(src, out, err) != 0) return -1;
        out->pat_str   = ray_str_ptr(cv_like);
        out->pat_len   = ray_str_len(cv_like);
        out->pat_compiled = ray_glob_compile(out->pat_str, out->pat_len);
        out->like_empty_match = (out->pat_compiled.shape != RAY_GLOB_SHAPE_NONE)
            ? (uint8_t)ray_glob_match_compiled(&out->pat_compiled, "", 0)
            : (uint8_t)ray_glob_match("", 0, out->pat_str, out->pat_len);
        if (src->type == RAY_SYM) {
            /* Cell ids are positions in the COLUMN's domain.  Runtime
             * domain: borrow the global string snapshot (lock-free per
             * row).  FILE domain: size the LUT by the domain's count and
             * resolve lazily via ray_sym_domain_str at eval over like_dom,
             * the source's domain (an encoded child has no col_obj).
             * Pre-flip this is always the runtime branch — exact no-op. */
            out->like_dom = src->dom;
            if (src->dom == ray_sym_runtime_domain()) {
                ray_t** sym_strings = NULL;
                uint32_t sym_count = 0;
                ray_sym_strings_borrow(&sym_strings, &sym_count);
                if (!sym_strings || sym_count == 0) return -1;
                uint8_t* lut = (uint8_t*)scratch_calloc(&out->aux_hdr, sym_count);
                if (!lut) return -1;
                out->like_lut = lut;
                out->like_lut_count = sym_count;
                out->like_sym_strings = sym_strings;
            } else {
                int64_t dn = ray_sym_domain_count(src->dom);
                if (dn <= 0 || dn > UINT32_MAX) return -1;
                uint8_t* lut = (uint8_t*)scratch_calloc(&out->aux_hdr, (size_t)dn);
                if (!lut) return -1;
                out->like_lut = lut;
                out->like_lut_count = (uint32_t)dn;
                out->like_sym_strings = NULL;  /* resolve via like_dom */
                out->like_raw_ok = ray_sym_domain_raw_pin(out->like_dom, &out->like_raw) ? 1 : 0;
            }
        }
        out->cval_in_dict = 1;
        return 0;
    }

    /* Dict-encoded STR EQ/NE: drive the comparison on the column's int32
     * dict-code vector (like a W32 SYM).  Resolve the constant string to
     * its code once; absent => EQ all-false / NE all-true via fold.  A
     * source without a dictionary (-2) takes the typed leg below. */
    if (src->type == RAY_STR && (out->op == FP_EQ || out->op == FP_NE)
        && rext->literal && rext->literal->type == -RAY_STR) {
        ray_t* cv_str = rext->literal;
        int64_t code = ray_col_src_dict_code(src, ray_str_ptr(cv_str), ray_str_len(cv_str));
        if (code != -2) {              /* never for an encoded source (no dictionary) */
            ray_t* col = NULL;
            if (fp_src_plain(src, &col, err) != 0) return -1;
            ray_index_t* dix = ray_index_payload(col->index);
            ray_t* codes = dix->u.dict.codes;
            if (!codes || RAY_IS_ERR(codes) || codes->len != col->len) return -1;
            out->col_type     = RAY_I32;        /* compare codes as int32 */
            out->col_attrs    = 0;
            out->col_esz      = 4;
            out->col_base     = ray_data(codes);
            out->col_obj      = codes;
            out->col_len      = col->len;
            out->cval         = (code >= 0) ? code : 0;
            out->cval_in_dict = (code >= 0) ? 1 : 0;
            if (code < 0)
                out->fold = (out->op == FP_EQ) ? FP_FOLD_FALSE : FP_FOLD_TRUE;
            return 0;
        }
        /* a generation-2 single-file column with a persisted DICT (P6b),
         * materialized or not: its codes, read from the mapping by range
         * (fp_bind_view); "" is an entry, so the null rows compare exactly */
        ray_enccol_t* de = (src->obj && src->obj->type == RAY_ENCCOL) ? ray_enccol_of(src->obj) : NULL;
        if (de && de->kind == RAY_ENCCOL_SINGLE && !de->synthetic && de->hdr.index_kind == RAY_IDX_DICT) {
            int64_t dc = -2;
            ray_t* ce = ray_enccol_idict_code(de, ray_str_ptr(cv_str), ray_str_len(cv_str), &dc);
            if (ce) { *err = ce; return -1; }
            out->col_type = RAY_I32; out->col_attrs = 0; out->col_esz = 4;
            out->col_base = NULL; out->col_obj = NULL; out->col_len = src->len;
            out->typed_compare = 0;
            out->enc = true; out->idict = de;
            out->cval = dc >= 0 ? dc : 0;
            out->cval_in_dict = dc >= 0;
            if (dc < 0) out->fold = (out->op == FP_EQ) ? FP_FOLD_FALSE : FP_FOLD_TRUE;
            return 0;
        }
    }

    /* Ordering ops on SYM are meaningless (dict ID order != string order). */
    if (src->type == RAY_SYM && (out->op == FP_LT || out->op == FP_LE ||
                                 out->op == FP_GT || out->op == FP_GE))
        return -1;
    /* Admission and execution share the typed comparison/null contract. */
    if (!fp_src_supported_op(src, out->op == FP_EQ || out->op == FP_NE))
        return -1;

    ray_t* cv = rext->literal;
    /* Atom type ↔ column class compatibility — block mixed-temporal
     * forms like `(== date_col timestamp_const)` whose raw-unit
     * comparison is meaningless. */
    if (!fp_atom_col_compatible(cv->type, src->type)) return -1;
    if (fp_bind_fields(src, out, err) != 0) return -1;

    out->literal = cv;
    out->constant_null = RAY_ATOM_IS_NULL(cv);
    out->float_compare = src->type == RAY_F32 || src->type == RAY_F64 || cv->type == -RAY_F32 || cv->type == -RAY_F64;
    out->typed_compare = src->type == RAY_STR || src->type == RAY_GUID ||
        (src->type != RAY_SYM && (out->float_compare || out->constant_null || src->may_null));
    if (src->type == RAY_STR || src->type == RAY_GUID) return 0;

    if (out->col_type == RAY_SYM) {
        /* The constant must be expressed in the COLUMN's domain — cells
         * below compare as raw indices against cval (sym-domain Phase 2).
         * Absent from the domain ⇒ matches nothing (EQ all-false / NE
         * all-true fold via cval_in_dict).  Exact no-op while the column
         * is runtime-domain: a runtime atom's id IS the cell-id space. */
        if (cv->type == -RAY_SYM) {
            if (src->dom == ray_sym_runtime_domain()) {
                out->cval = cv->i64;
                out->cval_in_dict = 1;
            } else {
                ray_t* cs = ray_sym_str(cv->i64);  /* atom: runtime id */
                int64_t did = cs ? ray_col_src_sym_find(src, ray_str_ptr(cs),
                                                         ray_str_len(cs)) : -1;
                out->cval = (did >= 0) ? did : 0;
                out->cval_in_dict = (did >= 0) ? 1 : 0;
            }
        } else if (cv->type == -RAY_STR) {
            int64_t did = ray_col_src_sym_find(src, ray_str_ptr(cv), ray_str_len(cv));
            out->cval = (did >= 0) ? did : 0;
            out->cval_in_dict = (did >= 0) ? 1 : 0;
        } else {
            return -1;
        }
    } else {
        /* Numeric / temporal: decode the atom into an int64 via the same
         * type-aware reader used elsewhere in the engine.  Atom type
         * has already been validated against col_type by
         * fp_atom_col_compatible above, so each branch knows the
         * stored unit matches the column's. */
        switch (cv->type) {
        case -RAY_F32: case -RAY_F64: out->fval = cv->f64; break;
        case -RAY_I64:       case -RAY_TIMESTAMP: out->cval = cv->i64;            break;
        case -RAY_I32:       case -RAY_DATE:
        case -RAY_TIME:                            out->cval = (int64_t)cv->i32;  break;
        case -RAY_I16:                             out->cval = (int64_t)cv->i16;  break;
        case -RAY_BOOL:      case -RAY_U8:         out->cval = (int64_t)cv->b8;   break;
        default: return -1;
        }
        out->cval_in_dict = 1;
    }

    if (out->typed_compare) {
        if (cv->type != -RAY_F32 && cv->type != -RAY_F64) out->fval = (double)out->cval;
        return 0;
    }

    /* Range-check cval against the column's representable range and
     * pre-fold the comparison when the constant lies outside it.
     * Without this, the inner-loop cast `(T)cval` silently truncates and
     * yields wrong results (e.g. `u8_col == 300` matched value 44).
     *
     * For SYM, the storage IDs are unsigned and bounded by 2^(8*esz);
     * a global sym ID larger than that can't appear in this column.
     * For numeric, U8/BOOL is unsigned [0..255], I16/I32/I64 are signed. */
    int64_t v_min, v_max;
    int     is_unsigned;
    if (out->col_type == RAY_SYM) {
        is_unsigned = 1;
        v_min = 0;
        switch (out->col_esz) {
        case 1: v_max = 0xFFLL;       break;
        case 2: v_max = 0xFFFFLL;     break;
        case 4: v_max = 0xFFFFFFFFLL; break;
        default: v_max = INT64_MAX;   break;
        }
    } else {
        switch (out->col_esz) {
        case 1: is_unsigned = 1; v_min = 0;          v_max = 0xFFLL;     break; /* U8/BOOL */
        case 2: is_unsigned = 0; v_min = INT16_MIN;  v_max = INT16_MAX;  break;
        case 4: is_unsigned = 0; v_min = INT32_MIN;  v_max = INT32_MAX;  break;
        default: is_unsigned = 0; v_min = INT64_MIN; v_max = INT64_MAX;  break;
        }
    }
    (void)is_unsigned;

    if (out->cval < v_min || out->cval > v_max) {
        int below = (out->cval < v_min);
        switch (out->op) {
        case FP_EQ: out->fold = FP_FOLD_FALSE; break;
        case FP_NE: out->fold = FP_FOLD_TRUE;  break;
        case FP_LT: out->fold = below ? FP_FOLD_FALSE : FP_FOLD_TRUE;  break; /* col < below ⇒ false; col < above ⇒ true */
        case FP_LE: out->fold = below ? FP_FOLD_FALSE : FP_FOLD_TRUE;  break;
        case FP_GT: out->fold = below ? FP_FOLD_TRUE  : FP_FOLD_FALSE; break;
        case FP_GE: out->fold = below ? FP_FOLD_TRUE  : FP_FOLD_FALSE; break;
        case FP_LIKE: case FP_IN: break;
        }
    }
    return 0;
}

/* Walk the predicate DAG (an OP_AND tree of leaf comparisons) and collect
 * leaves into `out->children`.  Pass 3: balanced binary OP_AND emitted
 * by compile_expr_dag means we recurse on both inputs whenever we see an
 * OP_AND node.  Returns 0 on success, -1 if a leaf can't be compiled or
 * the fan-in exceeds FP_PRED_MAX_CHILDREN. */
static int fp_compile_pred_dag(ray_graph_t* g, ray_op_t* node, ray_t* tbl,
                               fp_pred_t* out, ray_t** err)
{
    if (!node) return -1;
    if (node->opcode == OP_AND) {
        if (fp_compile_pred_dag(g, op_child(g, node, 0), tbl, out, err) != 0) return -1;
        if (fp_compile_pred_dag(g, op_child(g, node, 1), tbl, out, err) != 0) return -1;
        return 0;
    }
    if (out->n_children >= FP_PRED_MAX_CHILDREN) return -1;
    fp_cmp_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (fp_compile_cmp(g, node, tbl, &tmp, err) != 0) {
        if (tmp.aux_hdr) scratch_free(tmp.aux_hdr);
        return -1;
    }
    out->children[out->n_children++] = tmp;
    return 0;
}

static int fp_cmp_selectivity_score(const fp_cmp_t* c) {
    if (c->fold == FP_FOLD_FALSE) return 0;
    if (c->op == FP_EQ && c->col_esz >= 8) return 1;
    if (c->op == FP_EQ) return 2;
    if (c->op == FP_IN) return 3;
    if (c->op == FP_LT || c->op == FP_LE || c->op == FP_GT || c->op == FP_GE)
        return 4;
    if (c->op == FP_NE) return 5;
    return 6;
}

static void fp_pred_order_children(fp_pred_t* p) {
    for (uint8_t i = 1; i < p->n_children; i++) {
        fp_cmp_t v = p->children[i];
        int vs = fp_cmp_selectivity_score(&v);
        uint8_t j = i;
        while (j > 0 && fp_cmp_selectivity_score(&p->children[j - 1]) > vs) {
            p->children[j] = p->children[j - 1];
            j--;
        }
        p->children[j] = v;
    }
}

int fp_compile_pred(ray_graph_t* g, ray_op_t* pred_op, ray_t* tbl,
                    fp_pred_t* out, ray_t** err)
{
    *err = NULL;
    memset(out, 0, sizeof(*out));
    out->n_children = 0;
    /* No predicate → const-true.  fp_eval_pred memsets bits to 1
     * when n_children == 0, so the worker treats every row as a hit. */
    if (!pred_op) return 0;
    int rc = fp_compile_pred_dag(g, pred_op, tbl, out, err);
    if (rc == 0 && out->n_children > 1)
        fp_pred_order_children(out);
    return rc;
}

void fp_pred_cleanup(fp_pred_t* p) {
    if (!p) return;
    for (uint8_t i = 0; i < p->n_children; i++) {
        if (p->children[i].like_raw_ok) {
            ray_sym_domain_raw_unpin(p->children[i].like_dom);
            p->children[i].like_raw_ok = 0;
        }
        if (p->children[i].aux_hdr) {
            scratch_free(p->children[i].aux_hdr);
            p->children[i].aux_hdr = NULL;
            p->children[i].like_lut = NULL;
            p->children[i].like_lut_count = 0;
            p->children[i].like_sym_strings = NULL;
        }
    }
}
