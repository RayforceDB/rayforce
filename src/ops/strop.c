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

#include "lang/internal.h"
#include "ops/internal.h"
#include "table/sym.h"
#include "table/domain.h"   /* sym-domain resolution (Phase 2) */
#include "table/table.h"
#include "ops/glob.h"
#include "lang/format.h"   /* ray_type_name (error context) */
#include <ctype.h>

/* ══════════════════════════════════════════
 * String builtins
 * ══════════════════════════════════════════ */

static bool strlen_atom_value(ray_t* x, int64_t* out) {
    if (x->type == -RAY_STR) {
        *out = (int64_t)ray_str_len(x);
        return true;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        *out = s ? (int64_t)ray_str_len(s) : 0;
        return true;
    }
    return false;
}

static bool strlen_vec_value(ray_t* x, int64_t row, int64_t* out) {
    if (x->type == RAY_STR) {
        size_t slen = 0;
        const char* s = ray_str_vec_get(x, row, &slen);
        *out = s ? (int64_t)slen : 0;
        return true;
    }
    if (x->type == RAY_SYM) {
        /* cell id: resolve through the COLUMN's domain (sym-domain Phase 2) */
        ray_t* s = ray_sym_vec_cell(x, row);
        *out = s ? (int64_t)ray_str_len(s) : 0;
        return true;
    }
    return false;
}

static ray_t* strlen_vec(ray_t* x) {
    if (x->type != RAY_STR && x->type != RAY_SYM)
        return ray_error("type", "strlen: expected string or symbol");

    int64_t n = x->len;
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;
    int64_t* dst = (int64_t*)ray_data(out);
    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(x, i)) {
            dst[i] = NULL_I64;
            out->attrs |= RAY_ATTR_HAS_NULLS;
        } else {
            strlen_vec_value(x, i, &dst[i]);
        }
    }
    return out;
}

static ray_t* strlen_mapcommon(ray_t* x) {
    ray_t** ptrs = (ray_t**)ray_data(x);
    ray_t* keys = ptrs[0];
    ray_t* counts = ptrs[1];
    if (!keys || !counts || RAY_IS_ERR(keys) || RAY_IS_ERR(counts))
        return ray_error("domain", "strlen: invalid partition column");
    if (keys->type != RAY_STR && keys->type != RAY_SYM)
        return ray_error("type", "strlen: expected string or symbol");

    int64_t total = ray_parted_nrows(x);
    ray_t* out = ray_vec_new(RAY_I64, total);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = total;
    int64_t* dst = (int64_t*)ray_data(out);
    const int64_t* cnt = (const int64_t*)ray_data(counts);

    int64_t off = 0;
    for (int64_t p = 0; p < counts->len; p++) {
        int64_t v = 0;
        bool is_null = ray_vec_is_null(keys, p);
        if (!is_null) strlen_vec_value(keys, p, &v);
        for (int64_t r = 0; r < cnt[p]; r++)
            dst[off++] = is_null ? NULL_I64 : v;
        if (is_null) out->attrs |= RAY_ATTR_HAS_NULLS;
    }
    return out;
}

static ray_t* strlen_parted(ray_t* x) {
    int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
    if (base != RAY_STR && base != RAY_SYM)
        return ray_error("type", "strlen: expected string or symbol");

    int64_t total = ray_parted_nrows(x);
    ray_t* out = ray_vec_new(RAY_I64, total);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = total;
    int64_t* dst = (int64_t*)ray_data(out);

    ray_t** segs = (ray_t**)ray_data(x);
    int64_t off = 0;
    for (int64_t s = 0; s < x->len; s++) {
        ray_t* seg = segs[s];
        if (!seg || RAY_IS_ERR(seg)) continue;
        for (int64_t i = 0; i < seg->len; i++) {
            if (ray_vec_is_null(seg, i)) {
                dst[off] = NULL_I64;
                out->attrs |= RAY_ATTR_HAS_NULLS;
            } else {
                strlen_vec_value(seg, i, &dst[off]);
            }
            off++;
        }
    }
    return out;
}

ray_t* ray_strlen_fn(ray_t* x) {
    if (ray_is_atom(x) && RAY_ATOM_IS_NULL(x))
        return ray_typed_null(-RAY_I64);
    int64_t len = 0;
    if (strlen_atom_value(x, &len)) return ray_i64(len);
    if (ray_is_vec(x)) return strlen_vec(x);
    if (x->type == RAY_MAPCOMMON) return strlen_mapcommon(x);
    if (RAY_IS_PARTED(x->type)) return strlen_parted(x);
    ray_t* el_ = ray_empty_list_as(x, RAY_STR);   /* (list): as the empty STR vector (#1790) */
    if (el_) { ray_t* r_ = ray_strlen_fn(el_); ray_release(el_); return r_; }
    return ray_error("type", "strlen: expected string or symbol");
}

static bool str_atom_bytes(ray_t* x, const char** out, size_t* out_len,
                           bool* out_sym) {
    if (x->type == -RAY_STR) {
        *out = ray_str_ptr(x);
        *out_len = ray_str_len(x);
        if (out_sym) *out_sym = false;
        return true;
    }
    if (x->type == -RAY_SYM) {
        ray_t* s = ray_sym_str(x->i64);
        if (!s) {
            *out = "";
            *out_len = 0;
        } else {
            *out = ray_str_ptr(s);
            *out_len = ray_str_len(s);
        }
        if (out_sym) *out_sym = true;
        return true;
    }
    return false;
}

static bool str_atom_arg(ray_t* x, const char** out, size_t* out_len) {
    bool is_sym = false;
    return str_atom_bytes(x, out, out_len, &is_sym);
}

static bool intish_atom_value(ray_t* x, int64_t* out) {
    switch (x->type) {
    case -RAY_I64: case -RAY_I32: case -RAY_I16: case -RAY_U8:
    case -RAY_BOOL:
        *out = as_i64(x);
        return true;
    case -RAY_F64:
        *out = (int64_t)x->f64;
        return true;
    default:
        return false;
    }
}

static bool substr_count_arg_ok(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return false;
    if (ray_is_atom(x)) {
        int64_t tmp = 0;
        return intish_atom_value(x, &tmp);
    }
    if (!ray_is_vec(x)) return false;
    switch (x->type) {
    case RAY_I64: case RAY_I32:
        return true;
    default:
        return false;
    }
}

static ray_op_t* const_node_from_obj(ray_graph_t* g, ray_t* x) {
    if (ray_is_vec(x)) return ray_const_vec(g, x);
    if (ray_is_atom(x)) return ray_const_atom(g, x);
    return NULL;
}

static ray_t* string_unary_atom(ray_t* x, uint16_t opcode) {
    const char* sp = NULL;
    size_t sl = 0;
    bool is_sym = false;
    if (!str_atom_bytes(x, &sp, &sl, &is_sym))
        return ray_error("type", "string transform: expected string or symbol, got %s", ray_type_name(x->type));

    char sbuf[8192];
    char* buf = sbuf;
    ray_t* dyn_hdr = NULL;
    if (sl + 1 > sizeof(sbuf)) {
        buf = (char*)scratch_alloc(&dyn_hdr, sl + 1);
        if (!buf) return ray_error("oom", NULL);
    }

    size_t out_len = sl;
    if (opcode == OP_UPPER) {
        for (size_t i = 0; i < sl; i++) buf[i] = (char)toupper((unsigned char)sp[i]);
    } else if (opcode == OP_LOWER) {
        for (size_t i = 0; i < sl; i++) buf[i] = (char)tolower((unsigned char)sp[i]);
    } else {
        size_t start = 0, end = sl;
        while (start < sl && isspace((unsigned char)sp[start])) start++;
        while (end > start && isspace((unsigned char)sp[end - 1])) end--;
        out_len = end - start;
        memcpy(buf, sp + start, out_len);
    }

    ray_t* out;
    if (is_sym) {
        int64_t sid = ray_sym_intern(buf, out_len);
        out = sid >= 0 ? ray_sym(sid) : ray_error("oom", NULL);
    } else {
        out = ray_str(buf, out_len);
    }
    scratch_free(dyn_hdr);
    return out;
}

static ray_t* string_unary_fn(ray_t* x, uint16_t opcode,
                              ray_op_t* (*ctor)(ray_graph_t*, ray_op_t*)) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (ray_is_lazy(x)) {
        /* a handle to extend: x, or a fresh one when x is shared (#1860) */
        ray_t* own = ray_lazy_own(x);
        if (!own || RAY_IS_ERR(own)) return own ? own : ray_error("oom", NULL);
        ray_op_t* prev = RAY_LAZY_OP(own);
        ray_t* res = (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM))
            ? ray_error("type", "string transform: expected string or symbol vector")
            : ray_lazy_append(own, opcode);
        if (own != x) ray_release(own);
        return res;
    }
    if (ray_is_atom(x))
        return string_unary_atom(x, opcode);
    if (!ray_is_vec(x) || (x->type != RAY_STR && x->type != RAY_SYM)) {
        if (ray_is_empty_list(x)) { ray_retain(x); return x; }   /* (list) in, (list) out (#1790) */
        return ray_error("type", "string transform: expected string or symbol, got %s", ray_type_name(x->type));
    }

    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = in ? ctor(g, in) : NULL;
    if (!op) {
        ray_graph_free(g);
        return ray_error("oom", NULL);
    }
    return ray_lazy_wrap(g, op);
}

ray_t* ray_upper_fn(ray_t* x) {
    return string_unary_fn(x, OP_UPPER, ray_upper);
}

ray_t* ray_lower_fn(ray_t* x) {
    return string_unary_fn(x, OP_LOWER, ray_lower);
}

ray_t* ray_trim_fn(ray_t* x) {
    return string_unary_fn(x, OP_TRIM, ray_trim_op);
}

static ray_t* substr_atom(ray_t* str, ray_t* start_obj, ray_t* len_obj) {
    const char* sp = NULL;
    size_t sl = 0;
    bool is_sym = false;
    int64_t start = 0, amount = 0;
    if (!str_atom_bytes(str, &sp, &sl, &is_sym))
        return ray_error("type", "substr: expected string or symbol, got %s", ray_type_name(str->type));

    /* A null start or length propagates to the empty (null) string.  In
     * Rayforce a STR/SYM has no null distinct from ""/' (see
     * ray_atom_is_null_fn), so the empty value IS the null result — matching
     * the per-row null propagation in exec_substr.  Checked BEFORE the
     * 1-based->0-based `start - 1` below: a null integer atom is INT64_MIN,
     * so the subtraction would be signed-overflow undefined behaviour
     * (reported by UBSan at this line). */
    if (RAY_ATOM_IS_NULL(start_obj) || RAY_ATOM_IS_NULL(len_obj)) {
        if (is_sym) {
            int64_t sid = ray_sym_intern("", 0);
            return sid >= 0 ? ray_sym(sid) : ray_error("oom", NULL);
        }
        return ray_str("", 0);
    }

    if (!intish_atom_value(start_obj, &start))
        return ray_error("type", "substr: start must be an integer atom");
    if (!intish_atom_value(len_obj, &amount))
        return ray_error("type", "substr: length must be an integer atom");

    int64_t st = start - 1;
    if (st < 0) st = 0;
    if ((size_t)st >= sl) {
        if (is_sym) {
            int64_t sid = ray_sym_intern("", 0);
            return sid >= 0 ? ray_sym(sid) : ray_error("oom", NULL);
        }
        return ray_str("", 0);
    }
    if (amount < 0 || amount > (int64_t)(sl - (size_t)st))
        amount = (int64_t)sl - st;
    if (is_sym) {
        int64_t sid = ray_sym_intern(sp + st, (size_t)amount);
        return sid >= 0 ? ray_sym(sid) : ray_error("oom", NULL);
    }
    return ray_str(sp + st, (size_t)amount);
}

ray_t* ray_substr_fn(ray_t** args, int64_t n) {
    if (n != 3)
        return ray_error("arity", "substr: expected 3 args, got %lld", (long long)n);
    ray_t* str = args[0];
    ray_t* start = args[1];
    ray_t* len = args[2];
    if (!substr_count_arg_ok(start))
        return ray_error("type", "substr: start must be an integer atom or I64/I32 vector");
    if (!substr_count_arg_ok(len))
        return ray_error("type", "substr: length must be an integer atom or I64/I32 vector");

    if (ray_is_lazy(str)) {
        /* a handle to extend in place: str, or a fresh one when str is
         * shared (#1860); the result is owned either way */
        ray_t* own = ray_lazy_own(str);
        if (!own || RAY_IS_ERR(own)) return own ? own : ray_error("oom", NULL);
        if (own == str) ray_retain(own);
        ray_graph_t* g = RAY_LAZY_GRAPH(own);
        ray_op_t* prev = RAY_LAZY_OP(own);
        if (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM)) {
            ray_release(own);
            return ray_error("type", "substr: expected string or symbol vector");
        }
        ray_op_t* s = const_node_from_obj(g, start);
        ray_op_t* l = const_node_from_obj(g, len);
        ray_op_t* op = (s && l) ? ray_substr(g, prev, s, l) : NULL;
        if (!op) { ray_release(own); return ray_error("oom", NULL); }
        RAY_LAZY_OP(own) = op;
        return own;
    }

    if (ray_is_atom(str))
        return substr_atom(str, start, len);

    if (!ray_is_vec(str) || (str->type != RAY_STR && str->type != RAY_SYM))
        return ray_error("type", "substr: expected string or symbol, got %s", ray_type_name(str->type));
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, str);
    ray_op_t* s = const_node_from_obj(g, start);
    ray_op_t* l = const_node_from_obj(g, len);
    ray_op_t* op = (in && s && l) ? ray_substr(g, in, s, l) : NULL;
    if (!op) {
        ray_graph_free(g);
        return ray_error("oom", NULL);
    }
    return ray_lazy_wrap(g, op);
}

static ray_t* replace_atom(ray_t* str, ray_t* from, ray_t* to) {
    const char* sp = NULL;
    const char* fp = NULL;
    const char* tp = NULL;
    size_t sl = 0, fl = 0, tl = 0;
    bool is_sym = false;
    if (!str_atom_bytes(str, &sp, &sl, &is_sym))
        return ray_error("type", "replace: expected string or symbol input, got %s", ray_type_name(str->type));
    if (!str_atom_arg(from, &fp, &fl))
        return ray_error("type", "replace: from must be string or symbol atom");
    if (!str_atom_arg(to, &tp, &tl))
        return ray_error("type", "replace: to must be string or symbol atom");

    size_t n_matches = (fl > 0) ? sl / fl : 0;
    size_t worst;
    if (fl > 0 && tl > fl && n_matches > SIZE_MAX / tl)
        return ray_error("oom", NULL);
    if (fl > 0 && tl >= fl)
        worst = n_matches * tl + (sl % fl) + 1;
    else
        worst = sl + 1;

    char sbuf[8192];
    char* buf = sbuf;
    ray_t* dyn_hdr = NULL;
    if (worst > sizeof(sbuf)) {
        buf = (char*)scratch_alloc(&dyn_hdr, worst);
        if (!buf) return ray_error("oom", NULL);
    }

    size_t bi = 0;
    for (size_t j = 0; j < sl; ) {
        if (fl > 0 && j + fl <= sl && memcmp(sp + j, fp, fl) == 0) {
            memcpy(buf + bi, tp, tl);
            bi += tl;
            j += fl;
        } else {
            buf[bi++] = sp[j++];
        }
    }

    ray_t* out;
    if (is_sym) {
        int64_t sid = ray_sym_intern(buf, bi);
        out = sid >= 0 ? ray_sym(sid) : ray_error("oom", NULL);
    } else {
        out = ray_str(buf, bi);
    }
    scratch_free(dyn_hdr);
    return out;
}

ray_t* ray_replace_fn(ray_t** args, int64_t n) {
    if (n != 3)
        return ray_error("arity", "replace: expected 3 args, got %lld", (long long)n);
    ray_t* str = args[0];
    ray_t* from = args[1];
    ray_t* to = args[2];
    const char* tmp_s = NULL;
    size_t tmp_len = 0;
    if (!str_atom_arg(from, &tmp_s, &tmp_len))
        return ray_error("type", "replace: from must be string or symbol atom");
    if (!str_atom_arg(to, &tmp_s, &tmp_len))
        return ray_error("type", "replace: to must be string or symbol atom");

    if (ray_is_lazy(str)) {
        /* a handle to extend in place: str, or a fresh one when str is
         * shared (#1860); the result is owned either way */
        ray_t* own = ray_lazy_own(str);
        if (!own || RAY_IS_ERR(own)) return own ? own : ray_error("oom", NULL);
        if (own == str) ray_retain(own);
        ray_graph_t* g = RAY_LAZY_GRAPH(own);
        ray_op_t* prev = RAY_LAZY_OP(own);
        if (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM)) {
            ray_release(own);
            return ray_error("type", "replace: expected string or symbol vector");
        }
        ray_op_t* f = const_node_from_obj(g, from);
        ray_op_t* t = const_node_from_obj(g, to);
        ray_op_t* op = (f && t) ? ray_replace(g, prev, f, t) : NULL;
        if (!op) { ray_release(own); return ray_error("oom", NULL); }
        RAY_LAZY_OP(own) = op;
        return own;
    }

    if (ray_is_atom(str))
        return replace_atom(str, from, to);

    if (!ray_is_vec(str) || (str->type != RAY_STR && str->type != RAY_SYM))
        return ray_error("type", "replace: expected string or symbol, got %s", ray_type_name(str->type));
    ray_graph_t* g = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, str);
    ray_op_t* f = const_node_from_obj(g, from);
    ray_op_t* t = const_node_from_obj(g, to);
    ray_op_t* op = (in && f && t) ? ray_replace(g, in, f, t) : NULL;
    if (!op) {
        ray_graph_free(g);
        return ray_error("oom", NULL);
    }
    return ray_lazy_wrap(g, op);
}

typedef struct { const char* p; size_t n; } str_needle_t;

static int64_t str_find_index(const char* hay, size_t hay_len,
                              const str_needle_t* pattern,
                              ray_pool_t* pool, bool* cancelled) {
    if (cancelled) *cancelled = false;
    if (pool_cancelled(pool)) {
        if (cancelled) *cancelled = true;
        return NULL_I64;
    }
    size_t pos = 0;
    return ray_str_find_literal(hay, hay_len, pattern->p, pattern->n, &pos)
               ? (int64_t)pos : NULL_I64;
}

static bool str_vec_cell_bytes(ray_t* x, int64_t row,
                               const char** out, size_t* out_len) {
    if (x->type == RAY_STR) {
        const char* s = ray_str_vec_get(x, row, out_len);
        if (!s) {
            *out = "";
            if (out_len) *out_len = 0;
        } else {
            *out = s;
        }
        return true;
    }
    if (x->type == RAY_SYM) {
        ray_t* s = ray_sym_vec_cell(x, row);
        *out = s ? ray_str_ptr(s) : "";
        if (out_len) *out_len = s ? ray_str_len(s) : 0;
        return true;
    }
    return false;
}

typedef struct {
    ray_t* hay;
    const str_needle_t* pattern;
    int64_t* out;
    ray_pool_t* pool;
    _Atomic(int) any_null;
    /* a SYM haystack of the global table: its atoms are read in blocks,
     * under one lock per block instead of one per cell against every
     * other worker */
    bool sym_runtime;
} str_find_vec_ctx_t;

#define STR_FIND_BLOCK 256
static void str_find_vec_task(void* vctx, uint32_t worker_id,
                              int64_t lo, int64_t hi) {
    (void)worker_id;
    str_find_vec_ctx_t* c = (str_find_vec_ctx_t*)vctx;
    bool any_null = false;
    int64_t ids[STR_FIND_BLOCK];
    ray_t* atoms[STR_FIND_BLOCK];
    for (int64_t b = lo; b < hi; b += STR_FIND_BLOCK) {
        if (pool_cancelled(c->pool)) return;
        int n = (int)(hi - b < STR_FIND_BLOCK ? hi - b : STR_FIND_BLOCK);
        if (c->sym_runtime) {
            for (int j = 0; j < n; j++)
                ids[j] = ray_read_sym(ray_data(c->hay), b + j, RAY_SYM, c->hay->attrs);
            ray_sym_str_batch(ids, n, atoms);
        }
        for (int j = 0; j < n; j++) {
            const char* hp = "";
            size_t hl = 0;
            if (c->sym_runtime) {
                if (atoms[j]) { hp = ray_str_ptr(atoms[j]); hl = ray_str_len(atoms[j]); }
            } else {
                str_vec_cell_bytes(c->hay, b + j, &hp, &hl);
            }
            bool cancelled = false;
            int64_t idx = str_find_index(hp, hl, c->pattern, c->pool, &cancelled);
            if (cancelled) return;
            c->out[b + j] = idx;
            if (idx == NULL_I64) any_null = true;
        }
    }
    if (any_null)
        atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}

static ray_t* str_find_vec(ray_t* hay, const str_needle_t* pattern) {
    int64_t n = hay->len;
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;

    ray_pool_t* pool = n >= RAY_PARALLEL_THRESHOLD ? ray_pool_get() : NULL;
    int64_t* dst = (int64_t*)ray_data(out);
    str_find_vec_ctx_t ctx = {
        .hay = hay,
        .pattern = pattern,
        .out = dst,
        .pool = pool,
        .sym_runtime = hay->type == RAY_SYM &&
                       ray_sym_vec_domain(hay) == ray_sym_runtime_domain()
    };
    atomic_init(&ctx.any_null, 0);

    if (pool && ray_pool_total_workers(pool) >= 2) {
        ray_pool_dispatch(pool, str_find_vec_task, &ctx, n);
    } else {
        str_find_vec_task(&ctx, 0, 0, n);
    }

    if (pool_cancelled(pool)) {
        ray_release(out);
        return ray_error("cancel", NULL);
    }
    if (atomic_load_explicit(&ctx.any_null, memory_order_relaxed))
        out->attrs |= RAY_ATTR_HAS_NULLS;
    return out;
}

static ray_t* str_find_impl(ray_t* hay, ray_t* pattern);

/* str-find is lazy-aware for its haystack only (a lazy one extends its
 * chain).  A lazy pattern is borrowed, and ray_lazy_materialize consumes
 * its argument: materialise an owned reference, never the caller's (#1423;
 * consuming the borrowed one freed the caller's value under it). */
ray_t* ray_str_find_fn(ray_t* hay, ray_t* pattern) {
    if (!ray_is_lazy(pattern)) return str_find_impl(hay, pattern);
    ray_retain(pattern);
    ray_t* pat = ray_lazy_materialize(pattern);
    if (!pat || RAY_IS_ERR(pat)) return pat ? pat : ray_error("oom", NULL);
    ray_t* out = str_find_impl(hay, pat);
    ray_release(pat);
    return out;
}

static ray_t* str_find_impl(ray_t* hay, ray_t* pattern) {
    if (!hay || RAY_IS_ERR(hay)) return hay;
    if (!pattern || RAY_IS_ERR(pattern)) return pattern;

    const char* np = NULL;
    size_t nl = 0;
    if (!str_atom_arg(pattern, &np, &nl))
        return ray_error("type", "str-find: pattern must be string or symbol atom");
    str_needle_t search = { np, nl };

    if (ray_is_lazy(hay)) {
        /* a handle to extend in place: hay, or a fresh one when hay is
         * shared (#1860); the result is owned either way */
        ray_t* own = ray_lazy_own(hay);
        if (!own || RAY_IS_ERR(own)) return own ? own : ray_error("oom", NULL);
        if (own == hay) ray_retain(own);
        ray_graph_t* g = RAY_LAZY_GRAPH(own);
        ray_op_t* prev = RAY_LAZY_OP(own);
        if (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM)) {
            ray_release(own);
            return ray_error("type", "str-find: expected string or symbol vector");
        }
        ray_op_t* p = const_node_from_obj(g, pattern);
        ray_op_t* op = p ? ray_str_find_op(g, prev, p) : NULL;
        if (!op) { ray_release(own); return ray_error("oom", NULL); }
        RAY_LAZY_OP(own) = op;
        return own;
    }

    const char* hp = NULL;
    size_t hl = 0;
    if (str_atom_bytes(hay, &hp, &hl, NULL)) {
        bool cancelled = false;
        int64_t idx = str_find_index(hp, hl, &search, NULL, &cancelled);
        if (cancelled) return ray_error("cancel", NULL);
        return idx == NULL_I64 ? ray_typed_null(-RAY_I64) : ray_i64(idx);
    }

    if (ray_is_vec(hay) && (hay->type == RAY_STR || hay->type == RAY_SYM))
        return str_find_vec(hay, &search);

    if (hay->type == RAY_LIST) {
        int64_t n = hay->len;
        ray_t* out = ray_vec_new(RAY_I64, n);
        if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
        out->len = n;
        int64_t* dst = (int64_t*)ray_data(out);
        ray_t** items = (ray_t**)ray_data(hay);
        bool any_null = false;
        for (int64_t i = 0; i < n; i++) {
            if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) {
                ray_release(out);
                return ray_error("cancel", NULL);
            }
            if (!str_atom_bytes(items[i], &hp, &hl, NULL)) {
                ray_release(out);
                return ray_error("type", "str-find: list items must be string or symbol atoms");
            }
            bool cancelled = false;
            int64_t idx = str_find_index(hp, hl, &search, NULL, &cancelled);
            if (cancelled) {
                ray_release(out);
                return ray_error("cancel", NULL);
            }
            dst[i] = idx;
            if (idx == NULL_I64) any_null = true;
        }
        if (any_null) out->attrs |= RAY_ATTR_HAS_NULLS;
        return out;
    }

    return ray_error("type", "str-find: expected string, symbol, string vector, symbol vector, or list");
}

static bool str_join_seq_len(ray_t* x, int64_t* n) {
    const char* sp = NULL;
    size_t sl = 0;
    if (str_atom_arg(x, &sp, &sl)) {
        *n = 1;
        return true;
    }
    if (x->type == RAY_STR || x->type == RAY_SYM || x->type == RAY_LIST) {
        *n = x->len;
        return true;
    }
    return false;
}

static bool str_join_seq_item(ray_t* x, int64_t row,
                              const char** out, size_t* out_len) {
    if (str_atom_arg(x, out, out_len))
        return row == 0;
    if (x->type == RAY_STR || x->type == RAY_SYM)
        return str_vec_cell_bytes(x, row, out, out_len);
    if (x->type == RAY_LIST) {
        ray_t** items = (ray_t**)ray_data(x);
        return str_atom_arg(items[row], out, out_len);
    }
    return false;
}

static bool str_size_add(size_t* total, size_t add) {
    if (add > SIZE_MAX - *total) return false;
    *total += add;
    return true;
}

ray_t* ray_str_join_fn(ray_t* items, ray_t* delim) {
    /* Not lazy-aware: eval materialises both arguments before the call. */
    if (!items || RAY_IS_ERR(items)) return items;
    if (!delim || RAY_IS_ERR(delim)) return delim;

    const char* dp = NULL;
    size_t dl = 0;
    if (!str_atom_arg(delim, &dp, &dl))
        return ray_error("type", "str-join: delimiter must be string or symbol atom");

    int64_t n = 0;
    if (!str_join_seq_len(items, &n))
        return ray_error("type", "str-join: expected string/symbol atom, string/symbol vector, or list");
    if (n <= 0) return ray_str("", 0);

    size_t total = 0;
    for (int64_t i = 0; i < n; i++) {
        if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL))
            return ray_error("cancel", NULL);
        const char* sp = NULL;
        size_t sl = 0;
        if (!str_join_seq_item(items, i, &sp, &sl))
            return ray_error("type", "str-join: items must be string or symbol values");
        if (i > 0 && !str_size_add(&total, dl))
            return ray_error("oom", NULL);
        if (!str_size_add(&total, sl))
            return ray_error("oom", NULL);
    }
    if (total > (size_t)INT64_MAX)
        return ray_error("oom", NULL);

    char sbuf[8192];
    char* buf = sbuf;
    ray_t* dyn_hdr = NULL;
    if (total + 1 > sizeof(sbuf)) {
        if (total == SIZE_MAX) return ray_error("oom", NULL);
        buf = (char*)scratch_alloc(&dyn_hdr, total + 1);
        if (!buf) return ray_error("oom", NULL);
    }

    size_t off = 0;
    for (int64_t i = 0; i < n; i++) {
        if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) {
            scratch_free(dyn_hdr);
            return ray_error("cancel", NULL);
        }
        const char* sp = NULL;
        size_t sl = 0;
        str_join_seq_item(items, i, &sp, &sl);
        if (i > 0 && dl > 0) {
            memcpy(buf + off, dp, dl);
            off += dl;
        }
        if (sl > 0) {
            memcpy(buf + off, sp, sl);
            off += sl;
        }
    }
    buf[off] = '\0';
    ray_t* out = ray_str(buf, off);
    scratch_free(dyn_hdr);
    return out;
}

/* Positions of a split by indices.  A position is an offset into the data, so
 * a negative one (a null reads as the smallest integer) addressed memory before
 * it; one past the end addressed memory after it.  The string form always
 * clamped the latter, and now every form does; the former is an error. */
static ray_t* split_positions_check(int64_t* idx, int64_t n, int64_t total) {
    for (int64_t i = 0; i < n; i++) {
        if (idx[i] < 0)
            return ray_error("domain", "split: positions must not be negative or null, got %lld at index %lld",
                             (long long)idx[i], (long long)i);
        if (idx[i] > total) idx[i] = total;
    }
    return NULL;
}

/* The parts of one string at each occurrence of `dp`: a list of string
 * atoms.  A delimiter that is empty or longer than the input leaves the
 * input whole. */
static ray_t* split_bytes(const char* sp, size_t slen, const char* dp, size_t dlen) {
    ray_t* result = ray_list_new(8);
    if (!result || RAY_IS_ERR(result)) return result ? result : ray_error("oom", NULL);
    size_t start = 0;
    if (dlen > 0 && slen >= dlen) {
        for (size_t i = 0; i <= slen - dlen; ) {
            if (memcmp(sp + i, dp, dlen) == 0) {
                ray_t* part = ray_str(sp + start, i - start);
                if (!part || RAY_IS_ERR(part)) { ray_release(result); return part ? part : ray_error("oom", NULL); }
                result = ray_list_append_take(result, part);
                ray_release(part);
                if (RAY_IS_ERR(result)) return result;
                i += dlen;
                start = i;
            } else {
                i++;
            }
        }
    }
    ray_t* part = ray_str(sp + start, slen - start);
    if (!part || RAY_IS_ERR(part)) { ray_release(result); return part ? part : ray_error("oom", NULL); }
    result = ray_list_append_take(result, part);
    ray_release(part);
    return result;
}

/* Is `x` a collection split takes cell by cell: a string or symbol vector,
 * or a list (of string or symbol atoms)? */
static bool split_cells_input(ray_t* x) {
    return x->type == RAY_STR || x->type == RAY_SYM || x->type == RAY_LIST;
}

/* (split v delim) for a split_cells_input `v`: a list with one entry per
 * cell, the list of that cell's parts (what the cell, as an atom, splits
 * into). */
static ray_t* split_cells(ray_t* v, const char* dp, size_t dlen) {
    ray_t* out = ray_list_new(v->len);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    ray_t** items = v->type == RAY_LIST ? (ray_t**)ray_data(v) : NULL;
    for (int64_t i = 0; i < v->len; i++) {
        if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) {
            ray_release(out);
            return ray_error("cancel", NULL);
        }
        const char* sp = NULL;
        size_t sl = 0;
        if (!items) {
            str_vec_cell_bytes(v, i, &sp, &sl);   /* a STR or SYM vector: every cell has bytes */
        } else if (!str_atom_bytes(items[i], &sp, &sl, NULL)) {
            int8_t t = items[i]->type;
            ray_release(out);
            return ray_error("type", "split: list items must be string or symbol atoms, got %s",
                             ray_type_name(t));
        }
        ray_t* parts = split_bytes(sp, sl, dp, dlen);
        if (!parts || RAY_IS_ERR(parts)) { ray_release(out); return parts ? parts : ray_error("oom", NULL); }
        out = ray_list_append_take(out, parts);
        ray_release(parts);
        if (RAY_IS_ERR(out)) return out;
    }
    return out;
}

/* The cut positions of an I64, I32 or I16 vector, read in place: boxing
 * each as an atom could fail and was read through unchecked (#1851).  A
 * null cut reads as its sentinel, as the boxed atom's value did. */
static void split_read_cuts(ray_t* delim, int64_t n, int64_t* out) {
    const void* d = ray_data(delim);
    for (int64_t i = 0; i < n; i++)
        out[i] = delim->type == RAY_I64 ? ((const int64_t*)d)[i]
               : delim->type == RAY_I32 ? (int64_t)((const int32_t*)d)[i]
               :                          (int64_t)((const int16_t*)d)[i];
}

ray_t* ray_split_fn(ray_t* str, ray_t* delim) {
    /* List split: (split list indices) → list of sub-lists */
    if (str->type == RAY_LIST &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return RAY_NULL_OBJ; /* null for empty indices */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", "split: more than 256 cut positions, got %lld", (long long)nidx);
        split_read_cuts(delim, nidx, idx_buf);
        ray_t* bad_pos = split_positions_check(idx_buf, nidx, str->len);
        if (bad_pos) return bad_pos;
        int64_t total = str->len;
        ray_t** items = (ray_t**)ray_data(str);
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            /* Try to make a typed vector if all elements are same type */
            if (seglen > 0) {
                int8_t first_type = items[start]->type;
                int all_same = 1;
                for (int64_t j = start + 1; j < start + seglen && j < total; j++) {
                    if (items[j]->type != first_type) { all_same = 0; break; }
                }
                if (all_same && first_type < 0 && first_type != -RAY_STR) {
                    int8_t vtype = -first_type;
                    ray_t* vec = ray_vec_new(vtype, seglen);
                    if (!vec || RAY_IS_ERR(vec)) {   /* never a list segment in its place (#1851) */
                        ray_release(result);          /* owned here, with the segments made */
                        return vec ? vec : ray_error("oom", NULL);
                    }
                    {
                        vec->len = seglen;
                        for (int64_t j = 0; j < seglen && start + j < total; j++)
                            store_typed_elem(vec, j, items[start + j]);
                        result = ray_list_append_take(result, vec);
                        ray_release(vec);
                        if (RAY_IS_ERR(result)) return result;
                        continue;
                    }
                }
            }
            /* Heterogeneous or string segment: make a sub-list */
            ray_t* seg = ray_list_new(seglen);
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            for (int64_t j = 0; j < seglen && start + j < total; j++) {
                ray_retain(items[start + j]);
                seg = ray_list_append_take(seg, items[start + j]);
                ray_release(items[start + j]);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            }
            result = ray_list_append_take(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Vector/string split: (split vec/str indices) → list of sub-vectors/substrings */
    if ((ray_is_vec(str) || (ray_is_atom(str) && (-str->type) == RAY_STR)) &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return RAY_NULL_OBJ; /* null for empty indices */
        /* Extract indices as i64 */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", "split: more than 256 cut positions, got %lld", (long long)nidx);
        split_read_cuts(delim, nidx, idx_buf);
        ray_t* bad_pos = split_positions_check(idx_buf, nidx,
            ray_is_atom(str) ? (int64_t)ray_str_len(str) : str->len);
        if (bad_pos) return bad_pos;
        /* String split by indices */
        if (ray_is_atom(str) && (-str->type) == RAY_STR) {
            const char* sp2 = ray_str_ptr(str);
            size_t total = ray_str_len(str);
            ray_t* result = ray_list_new(nidx + 1);
            if (RAY_IS_ERR(result)) return result;
            for (int64_t i = 0; i < nidx; i++) {
                int64_t start = idx_buf[i];
                int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : (int64_t)total;
                int64_t seglen = end - start;
                if (seglen < 0) seglen = 0;
                if (start > (int64_t)total) start = (int64_t)total;
                if (start + seglen > (int64_t)total) seglen = (int64_t)total - start;
                ray_t* seg = ray_str(sp2 + start, (size_t)seglen);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
                result = ray_list_append_take(result, seg);
                ray_release(seg);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
        /* Vector split by indices */
        int64_t total = str->len;
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            /* a segment keeps the null flags and the strings of the source; a raw
             * copy of the cells dropped both (a null came out as its sentinel value,
             * a string that is not inline as an empty one) */
            ray_t* seg;
            if (str->type == RAY_STR) {
                /* A string segment is copied: a slice of a string vector shares the
                 * pool of the whole vector, so saving 200 segments wrote the pool
                 * 200 times (178x the size), each reload allocated it again, and
                 * every reader of ->str_pool had to know about slices. The copy
                 * holds only the bytes its own cells reference. */
                int64_t* rows = (int64_t*)ray_alloc_raw((size_t)(seglen > 0 ? seglen : 1) * sizeof(int64_t));
                if (!rows) { ray_release(result); return ray_error("oom", NULL); }
                for (int64_t r = 0; r < seglen; r++) rows[r] = start + r;
                seg = gather_by_idx(str, rows, seglen);
                ray_free_raw(rows);
            } else {
                seg = ray_vec_slice(str, start, seglen);
            }
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            result = ray_list_append_take(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* A string delimiter: an atom splits into its parts, a string or
     * symbol vector (flat or parted) or a list of such atoms splits each
     * cell, one list of parts per cell. */
    if (delim->type != -RAY_STR) {
        if (str->type != -RAY_STR && str->type != -RAY_SYM && !split_cells_input(str))
            return ray_error("type", "split: expected str or sym input, got %s", ray_type_name(str->type));
        return ray_error("type", "split: expected str delimiter, got %s", ray_type_name(delim->type));
    }
    const char* dp = ray_str_ptr(delim);
    size_t dlen = ray_str_len(delim);
    const char* sp = NULL;
    size_t slen = 0;
    if (str_atom_bytes(str, &sp, &slen, NULL))
        return split_bytes(sp, slen, dp, dlen);
    if (split_cells_input(str))
        return split_cells(str, dp, dlen);
    return ray_error("type", "split: expected str or sym input, got %s", ray_type_name(str->type));
}

/* (like str pattern) — glob-style pattern matching.
 * Syntax: * (any), ? (one char), [abc] / [a-z] / [!abc] (char class).
 * Implementation lives in src/ops/glob.[ch]; same matcher is used by
 * the DAG executor (string.c::exec_like) for select-where contexts. */
ray_t* ray_like_fn(ray_t* x, ray_t* pattern) {
    /* A string column matches row by row, as in a query (#1615); otherwise
     * the pattern is a string atom. */
    if (ray_like_pat_rows(pattern)) return ray_like_rows(x, pattern, false);
    if (pattern->type != -RAY_STR) {
        ray_t* el_ = ray_empty_list_as(pattern, RAY_STR);   /* (list): as the empty STR vector (#1790) */
        if (el_) { ray_t* r_ = ray_like_fn(x, el_); ray_release(el_); return r_; }
        return ray_error("type", "like: pattern must be a STR atom or a STR vector, got %s", ray_type_name(pattern->type));
    }
    const char* pat = ray_str_ptr(pattern);
    size_t pat_len = ray_str_len(pattern);

    /* Pre-compile the pattern once.  Most selective LIKE shapes are
     * `*literal*` (substring) which collapses to a memmem call — the
     * libc-provided implementation is SIMD on glibc/Apple/BSD.  When the
     * shape is RAY_GLOB_SHAPE_NONE we keep the iterative matcher. */
    ray_glob_compiled_t pc = ray_glob_compile(pat, pat_len);
    bool use_simple = pc.shape != RAY_GLOB_SHAPE_NONE;

    /* Atom: single match */
    if (x->type == -RAY_STR || x->type == -RAY_SYM) {
        const char* s; size_t sl;
        ray_t* sym_str = NULL;
        if (x->type == -RAY_SYM) {
            sym_str = ray_sym_str(x->i64);
            s  = sym_str ? ray_str_ptr(sym_str) : "";
            sl = sym_str ? ray_str_len(sym_str) : 0;
        } else {
            s  = ray_str_ptr(x);
            sl = ray_str_len(x);
        }
        bool m = use_simple ? ray_glob_match_compiled(&pc, s, sl)
                            : ray_glob_match(s, sl, pat, pat_len);
        if (sym_str) ray_release(sym_str);
        return make_bool(m ? 1 : 0);
    }

    /* Whole STR / SYM column, flat or parted: the kernel the executor
     * runs for `where: (like col pat)` — one pattern resolve per distinct
     * symbol, row passes over the worker pool (ops/string.c). */
    if (ray_is_vec(x) && (x->type == RAY_SYM || x->type == RAY_STR))
        return ray_like_vec(x, pattern, NULL);
    if (RAY_IS_PARTED(x->type)) {
        int8_t base = (int8_t)RAY_PARTED_BASETYPE(x->type);
        if (base == RAY_STR || RAY_IS_SYM(base))
            return ray_like_vec(x, pattern, NULL);
    }

    /* List of string/symbol atoms — the shape splayed string columns
     * load as (col_load_str_list).  Mirrors str-find's list branch. */
    if (x->type == RAY_LIST) {
        int64_t n = x->len;
        ray_t* result = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t* out = (uint8_t*)ray_data(result);
        ray_t** items = (ray_t**)ray_data(x);
        for (int64_t i = 0; i < n; i++) {
            if ((i & (RAY_MORSEL_ELEMS - 1)) == 0 && pool_cancelled(NULL)) {
                ray_release(result);
                return ray_error("cancel", NULL);
            }
            const char* s; size_t sl;
            if (!str_atom_bytes(items[i], &s, &sl, NULL)) {
                ray_release(result);
                return ray_error("type", "like: list items must be string or symbol atoms");
            }
            out[i] = (use_simple ? ray_glob_match_compiled(&pc, s, sl)
                                 : ray_glob_match(s, sl, pat, pat_len)) ? 1 : 0;
        }
        return result;
    }

    return ray_error("type", "like: expects string or symbol");
}

ray_t* ray_sym_name_fn(ray_t* x) {
    /* a query's private id (table/sym.h) is no symbol: refused at this
     * boundary by the one test for it, which states the rule here; today
     * ray_sym_str has no string for one either, so the check below it would
     * refuse it too, but that is the string table's layout, not a promise */
    if (x->type == -RAY_I64) {
        if (x->i64 < 0 || ray_sym_is_private(x->i64) || !ray_sym_str(x->i64))
            return ray_error("domain", "sym-name: invalid sym ID");
        return ray_sym(x->i64);
    }
    if (x->type == RAY_I64) {
        int64_t n = x->len;
        const int64_t* data = (const int64_t*)ray_data(x);
        /* Validate all IDs first */
        for (int64_t i = 0; i < n; i++) {
            if (data[i] < 0 || ray_sym_is_private(data[i]) || !ray_sym_str(data[i]))
                return ray_error("domain", "sym-name: invalid sym ID in vector");
        }
        ray_t* out = ray_vec_new(RAY_SYM, n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            out = ray_vec_append_take(out, &data[i]);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    /* Already sym (atom or vector), or empty I64/SYM vector — passthrough */
    if (x->type == -RAY_SYM || x->type == RAY_SYM ||
        ((x->type == RAY_I64 || x->type == RAY_SYM) && x->len == 0)) {
        ray_retain(x); return x;
    }
    ray_t* el_ = ray_empty_list_as(x, RAY_I64);   /* (list): as the empty I64 vector (#1790) */
    if (el_) { ray_t* r_ = ray_sym_name_fn(el_); ray_release(el_); return r_; }
    return ray_error("type", "sym-name: expected i64 or i64 vector");
}
