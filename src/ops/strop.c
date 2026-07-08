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
    /* STR/SYM have no null. */
    for (int64_t i = 0; i < n; i++)
        strlen_vec_value(x, i, &dst[i]);
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
        strlen_vec_value(keys, p, &v);  /* STR/SYM have no null */
        for (int64_t r = 0; r < cnt[p]; r++)
            dst[off++] = v;
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
        /* STR/SYM have no null. */
        for (int64_t i = 0; i < seg->len; i++) {
            strlen_vec_value(seg, i, &dst[off]);
            off++;
        }
    }
    return out;
}

ray_t* ray_strlen_fn(ray_t* x) {
    int64_t len = 0;
    if (strlen_atom_value(x, &len)) return ray_i64(len);
    if (ray_is_vec(x)) return strlen_vec(x);
    if (x->type == RAY_MAPCOMMON) return strlen_mapcommon(x);
    if (RAY_IS_PARTED(x->type)) return strlen_parted(x);
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
        ray_op_t* prev = RAY_LAZY_OP(x);
        if (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM))
            return ray_error("type", "string transform: expected string or symbol vector");
        return ray_lazy_append(x, opcode);
    }
    if (ray_is_atom(x))
        return string_unary_atom(x, opcode);
    if (!ray_is_vec(x) || (x->type != RAY_STR && x->type != RAY_SYM))
        return ray_error("type", "string transform: expected string or symbol, got %s", ray_type_name(x->type));

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
        ray_graph_t* g = RAY_LAZY_GRAPH(str);
        ray_op_t* prev = RAY_LAZY_OP(str);
        if (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM))
            return ray_error("type", "substr: expected string or symbol vector");
        ray_op_t* s = const_node_from_obj(g, start);
        ray_op_t* l = const_node_from_obj(g, len);
        ray_op_t* op = (s && l) ? ray_substr(g, prev, s, l) : NULL;
        if (!op) return ray_error("oom", NULL);
        RAY_LAZY_OP(str) = op;
        ray_retain(str);
        return str;
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
        ray_graph_t* g = RAY_LAZY_GRAPH(str);
        ray_op_t* prev = RAY_LAZY_OP(str);
        if (!prev || (prev->out_type != RAY_STR && prev->out_type != RAY_SYM))
            return ray_error("type", "replace: expected string or symbol vector");
        ray_op_t* f = const_node_from_obj(g, from);
        ray_op_t* t = const_node_from_obj(g, to);
        ray_op_t* op = (f && t) ? ray_replace(g, prev, f, t) : NULL;
        if (!op) return ray_error("oom", NULL);
        RAY_LAZY_OP(str) = op;
        ray_retain(str);
        return str;
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

static const char* str_bytes_find(const char* hay, size_t hay_len,
                                  const char* needle, size_t needle_len,
                                  ray_pool_t* pool, bool* cancelled) {
    if (cancelled) *cancelled = false;
    if (needle_len == 0) return hay;
    if (needle_len > hay_len) return NULL;

    const unsigned char first = (unsigned char)needle[0];
    const size_t chunk = (size_t)1 << 20;
    size_t base = 0;
    while (base + needle_len <= hay_len) {
        if (pool_cancelled(pool)) {
            if (cancelled) *cancelled = true;
            return NULL;
        }
        size_t starts = hay_len - base - needle_len + 1;
        size_t scan = starts < chunk ? starts : chunk;
        const void* hit = memchr(hay + base, first, scan);
        if (!hit) {
            base += scan;
            continue;
        }
        const char* p = (const char*)hit;
        size_t pos = (size_t)(p - hay);
        if (memcmp(p, needle, needle_len) == 0)
            return p;
        base = pos + 1;
    }
    return NULL;
}

static int64_t str_find_index(const char* hay, size_t hay_len,
                              const char* needle, size_t needle_len,
                              ray_pool_t* pool, bool* cancelled) {
    const char* p = str_bytes_find(hay, hay_len, needle, needle_len,
                                   pool, cancelled);
    if (cancelled && *cancelled) return NULL_I64;
    return p ? (int64_t)(p - hay) : NULL_I64;
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
    const char* needle;
    size_t needle_len;
    int64_t* out;
    ray_pool_t* pool;
    _Atomic(int) any_null;
} str_find_vec_ctx_t;

static void str_find_vec_task(void* vctx, uint32_t worker_id,
                              int64_t lo, int64_t hi) {
    (void)worker_id;
    str_find_vec_ctx_t* c = (str_find_vec_ctx_t*)vctx;
    bool any_null = false;
    for (int64_t i = lo; i < hi; i++) {
        if (((i - lo) & (RAY_MORSEL_ELEMS - 1)) == 0 &&
            pool_cancelled(c->pool))
            return;
        const char* hp = "";
        size_t hl = 0;
        str_vec_cell_bytes(c->hay, i, &hp, &hl);
        bool cancelled = false;
        int64_t idx = str_find_index(hp, hl, c->needle, c->needle_len,
                                     c->pool, &cancelled);
        if (cancelled) return;
        c->out[i] = idx;
        if (idx == NULL_I64) any_null = true;
    }
    if (any_null)
        atomic_store_explicit(&c->any_null, 1, memory_order_relaxed);
}

static ray_t* str_find_vec(ray_t* hay, const char* needle, size_t needle_len) {
    int64_t n = hay->len;
    ray_t* out = ray_vec_new(RAY_I64, n);
    if (!out || RAY_IS_ERR(out)) return out ? out : ray_error("oom", NULL);
    out->len = n;

    ray_pool_t* pool = n >= RAY_PARALLEL_THRESHOLD ? ray_pool_get() : NULL;
    int64_t* dst = (int64_t*)ray_data(out);
    str_find_vec_ctx_t ctx = {
        .hay = hay,
        .needle = needle,
        .needle_len = needle_len,
        .out = dst,
        .pool = pool
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

ray_t* ray_str_find_fn(ray_t* hay, ray_t* needle) {
    if (ray_is_lazy(hay)) hay = ray_lazy_materialize(hay);
    if (ray_is_lazy(needle)) needle = ray_lazy_materialize(needle);
    if (!hay || RAY_IS_ERR(hay)) return hay;
    if (!needle || RAY_IS_ERR(needle)) return needle;

    const char* np = NULL;
    size_t nl = 0;
    if (!str_atom_arg(needle, &np, &nl))
        return ray_error("type", "str-find: needle must be string or symbol atom");

    const char* hp = NULL;
    size_t hl = 0;
    if (str_atom_bytes(hay, &hp, &hl, NULL)) {
        bool cancelled = false;
        int64_t idx = str_find_index(hp, hl, np, nl, NULL, &cancelled);
        if (cancelled) return ray_error("cancel", NULL);
        return idx == NULL_I64 ? ray_typed_null(-RAY_I64) : ray_i64(idx);
    }

    if (ray_is_vec(hay) && (hay->type == RAY_STR || hay->type == RAY_SYM))
        return str_find_vec(hay, np, nl);

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
            int64_t idx = str_find_index(hp, hl, np, nl, NULL, &cancelled);
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
    if (ray_is_lazy(items)) items = ray_lazy_materialize(items);
    if (ray_is_lazy(delim)) delim = ray_lazy_materialize(delim);
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

ray_t* ray_split_fn(ray_t* str, ray_t* delim) {
    /* List split: (split list indices) → list of sub-lists */
    if (str->type == RAY_LIST &&
        ray_is_vec(delim) && (delim->type == RAY_I64 || delim->type == RAY_I16 || delim->type == RAY_I32)) {
        int64_t nidx = delim->len;
        if (nidx == 0) return RAY_NULL_OBJ; /* null for empty indices */
        int64_t idx_buf[256];
        if (nidx > 256) return ray_error("limit", NULL);
        for (int64_t ii = 0; ii < nidx; ii++) {
            int alloc = 0;
            ray_t* ie = collection_elem(delim, ii, &alloc);
            idx_buf[ii] = as_i64(ie);
            if (alloc) ray_release(ie);
        }
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
                    if (!RAY_IS_ERR(vec)) {
                        vec->len = seglen;
                        for (int64_t j = 0; j < seglen && start + j < total; j++)
                            store_typed_elem(vec, j, items[start + j]);
                        result = ray_list_append(result, vec);
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
                seg = ray_list_append(seg, items[start + j]);
                ray_release(items[start + j]);
                if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            }
            result = ray_list_append(result, seg);
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
        if (nidx > 256) return ray_error("limit", NULL);
        for (int64_t ii = 0; ii < nidx; ii++) {
            int alloc = 0;
            ray_t* ie = collection_elem(delim, ii, &alloc);
            idx_buf[ii] = as_i64(ie);
            if (alloc) ray_release(ie);
        }
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
                result = ray_list_append(result, seg);
                ray_release(seg);
                if (RAY_IS_ERR(result)) return result;
            }
            return result;
        }
        /* Vector split by indices */
        int64_t total = str->len;
        int esz = ray_elem_size(str->type);
        ray_t* result = ray_list_new(nidx + 1);
        if (RAY_IS_ERR(result)) return result;
        for (int64_t i = 0; i < nidx; i++) {
            int64_t start = idx_buf[i];
            int64_t end = (i + 1 < nidx) ? idx_buf[i + 1] : total;
            int64_t seglen = end - start;
            if (seglen < 0) seglen = 0;
            ray_t* seg = ray_vec_new(str->type, seglen);
            if (RAY_IS_ERR(seg)) { ray_release(result); return seg; }
            seg->len = seglen;
            if (seglen > 0) memcpy(ray_data(seg), (char*)ray_data(str) + start * esz, seglen * esz);
            result = ray_list_append(result, seg);
            ray_release(seg);
            if (RAY_IS_ERR(result)) return result;
        }
        return result;
    }
    /* Normalize str and delim to string pointers */
    const char *sp, *dp;
    size_t slen, dlen;
    ray_t* sym_str_s = NULL;
    ray_t* sym_str_d = NULL;
    if (str->type == -RAY_STR) { sp = ray_str_ptr(str); slen = ray_str_len(str); }
    else if (str->type == -RAY_SYM) { sym_str_s = ray_sym_str(str->i64); if (!sym_str_s) return ray_error("domain", "split: unresolved sym for input string"); sp = ray_str_ptr(sym_str_s); slen = ray_str_len(sym_str_s); }
    /* RAY_CHAR removed — all chars are now -RAY_STR */
    else return ray_error("type", "split: expected str or sym input, got %s", ray_type_name(str->type));
    if (delim->type == -RAY_STR) { dp = ray_str_ptr(delim); dlen = ray_str_len(delim); }
    /* RAY_CHAR removed — all chars are now -RAY_STR */
    else { int8_t dt = delim->type; if (sym_str_s) ray_release(sym_str_s); return ray_error("type", "split: expected str delimiter, got %s", ray_type_name(dt)); }

    ray_t* result = ray_list_new(8);
    if (RAY_IS_ERR(result)) { if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return result; }

    if (dlen == 0 || slen == 0) {
        ray_t* part = ray_str(sp, slen);
        result = ray_list_append(result, part);
        ray_release(part);
        if (sym_str_s) ray_release(sym_str_s);
        if (sym_str_d) ray_release(sym_str_d);
        return result;
    }

    size_t start = 0;
    for (size_t i = 0; i <= slen - dlen; ) {
        if (memcmp(sp + i, dp, dlen) == 0) {
            ray_t* part = ray_str(sp + start, i - start);
            if (RAY_IS_ERR(part)) { ray_release(result); if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return part; }
            result = ray_list_append(result, part);
            ray_release(part);
            if (RAY_IS_ERR(result)) { if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return result; }
            i += dlen;
            start = i;
        } else {
            i++;
        }
    }
    /* Last part */
    ray_t* part = ray_str(sp + start, slen - start);
    if (RAY_IS_ERR(part)) { ray_release(result); if (sym_str_s) ray_release(sym_str_s); if (sym_str_d) ray_release(sym_str_d); return part; }
    result = ray_list_append(result, part);
    ray_release(part);
    if (sym_str_s) ray_release(sym_str_s);
    if (sym_str_d) ray_release(sym_str_d);
    return result;
}

/* (like str pattern) — glob-style pattern matching.
 * Syntax: * (any), ? (one char), [abc] / [a-z] / [!abc] (char class).
 * Implementation lives in src/ops/glob.[ch]; same matcher is used by
 * the DAG executor (string.c::exec_like) for select-where contexts. */
ray_t* ray_like_fn(ray_t* x, ray_t* pattern) {
    /* Pattern must be a string atom */
    if (pattern->type != -RAY_STR) return ray_error("type", "like: pattern must be a string");
    const char* pat = ray_str_ptr(pattern);
    size_t pat_len = ray_str_len(pattern);

    /* Pre-compile the pattern once.  Most benchmark LIKE shapes are
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

    /* Vector: map over elements */
    if (ray_is_vec(x) && (x->type == RAY_SYM || x->type == RAY_STR)) {
        int64_t n = ray_len(x);
        ray_t* result = ray_vec_new(RAY_BOOL, n);
        if (RAY_IS_ERR(result)) return result;
        result->len = n;
        uint8_t* out = (uint8_t*)ray_data(result);

        if (x->type == RAY_SYM) {
            /* SYM column is dictionary-encoded with adaptive widths
             * (W8/W16/W32/W64).  Two bugs to avoid:
             *   (a) Reading the column as int64_t* is wrong for any
             *       width != W64 — must use ray_read_sym.
             *   (b) ray_sym_str returns a borrowed pointer; releasing
             *       it would decrement the global sym table entry.
             *
             * Fast path: a SYM column with N rows references at most
             * D = ray_sym_count() distinct sym_ids.  Build a
             * sym_id → bool LUT with a "seen" bitmap so each sym_id
             * runs the glob matcher at most once.  For LIKE on URL
             * (1.7M unique values, 5M rows) this turns an O(n_rows)
             * pattern-scan into O(n_distinct + n_rows) — the second
             * pass is a single byte load + table lookup per row. */
            const void* base = ray_data(x);
            int8_t in_type = x->type;
            uint8_t in_attrs = x->attrs;

            /* The global sym table can be much larger than the set of
             * IDs this column references (e.g. BrowserCountry with 54
             * uniques in a process that's also loaded URL with 1.7M
             * uniques).  Lazy-resolve via the seen bitmap so we only
             * match against sym_ids actually touched.  ray_sym_strings_borrow
             * snapshots the strings array under one lock so each lookup
             * is a plain pointer load. */
            /* Cell ids are positions in the COLUMN's domain (sym-domain
             * Phase 2).  Runtime: borrow the global snapshot (below).
             * FILE: size the LUT by the domain's count and resolve via
             * ray_sym_domain_str (fused_group precedent).  Pre-flip
             * this is always the runtime branch. */
            struct ray_sym_domain_s* dom = ray_sym_vec_domain(x);
            ray_t** sym_strings = NULL;
            uint32_t dict_n = 0;
            if (dom == ray_sym_runtime_domain()) {
                dom = NULL;
                ray_sym_strings_borrow(&sym_strings, &dict_n);
            } else {
                int64_t dn = ray_sym_domain_count(dom);
                dict_n = (dn > 0 && dn <= (int64_t)UINT32_MAX) ? (uint32_t)dn : 0;
            }
            ray_t* lut_hdr = NULL;
            ray_t* seen_hdr = NULL;
            uint8_t* lut = NULL;
            uint8_t* seen = NULL;
            if (dict_n > 0) {
                lut  = (uint8_t*)scratch_alloc (&lut_hdr,  (size_t)dict_n);
                seen = (uint8_t*)scratch_calloc(&seen_hdr, (size_t)dict_n);
            }
            if (lut && seen) {
                /* Out-of-range sym IDs ("unknown" or "missing") match
                 * the pattern as if the string were empty — same
                 * semantics as the OOM fallback at line 332 below and
                 * the atom case at line 217 above (sym_str==NULL → "").
                 * Compute the empty-string match once so the row pass
                 * can short-circuit out-of-range sids without taking
                 * the matcher per row. */
                uint8_t empty_match = (use_simple
                                       ? ray_glob_match_compiled(&pc, "", 0)
                                       : ray_glob_match("", 0, pat, pat_len))
                                      ? 1 : 0;

                /* First pass: discover the unique sym_ids referenced and
                 * resolve each pattern match exactly once.  Second pass:
                 * width-specialised LUT projection so the per-row loop
                 * is a tight gather. */
                int sym_w = (int)(in_attrs & RAY_SYM_W_MASK);
                #define DICT_PASS(LOAD)                                       \
                    for (int64_t i = 0; i < n; i++) {                         \
                        int64_t sid = (LOAD);                                 \
                        if ((uint64_t)sid >= (uint64_t)dict_n) continue;      \
                        if (!seen[sid]) {                                     \
                            ray_t* s = sym_strings ? sym_strings[sid]         \
                                     : ray_sym_domain_str(dom, sid);          \
                            const char* sp = s ? ray_str_ptr(s) : "";         \
                            size_t sl = s ? ray_str_len(s) : 0;               \
                            lut[sid] = (use_simple                            \
                                        ? ray_glob_match_compiled(&pc, sp, sl)\
                                        : ray_glob_match(sp, sl, pat, pat_len)) \
                                       ? 1 : 0;                               \
                            seen[sid] = 1;                                    \
                        }                                                     \
                    }
                #define ROW_PASS(LOAD)                                        \
                    for (int64_t i = 0; i < n; i++) {                         \
                        int64_t sid = (LOAD);                                 \
                        out[i] = ((uint64_t)sid < (uint64_t)dict_n)           \
                                 ? lut[sid]                                   \
                                 : empty_match;                               \
                    }
                switch (sym_w) {
                case RAY_SYM_W8: {
                    const uint8_t* d = (const uint8_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                case RAY_SYM_W16: {
                    const uint16_t* d = (const uint16_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                case RAY_SYM_W32: {
                    const uint32_t* d = (const uint32_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                case RAY_SYM_W64:
                default: {
                    const int64_t* d = (const int64_t*)base;
                    DICT_PASS(d[i]) ROW_PASS(d[i]) break;
                }
                }
                #undef DICT_PASS
                #undef ROW_PASS
                scratch_free(lut_hdr);
                scratch_free(seen_hdr);
            } else {
                /* OOM building the LUT: fall back to per-row scan.  Still
                 * uses ray_read_sym for adaptive-width correctness. */
                if (lut_hdr) scratch_free(lut_hdr);
                if (seen_hdr) scratch_free(seen_hdr);
                for (int64_t i = 0; i < n; i++) {
                    int64_t sid = ray_read_sym(base, i, in_type, in_attrs);
                    ray_t* s = dom ? ray_sym_domain_str(dom, sid)
                             : (sym_strings && (uint64_t)sid < (uint64_t)dict_n)
                               ? sym_strings[sid] : NULL;
                    const char* sp = s ? ray_str_ptr(s) : "";
                    size_t sl = s ? ray_str_len(s) : 0;
                    out[i] = (use_simple
                              ? ray_glob_match_compiled(&pc, sp, sl)
                              : ray_glob_match(sp, sl, pat, pat_len)) ? 1 : 0;
                }
            }
        } else {
            /* RAY_STR vector */
            for (int64_t i = 0; i < n; i++) {
                size_t slen;
                const char* s = ray_str_vec_get(x, i, &slen);
                bool m = false;
                if (s) {
                    m = use_simple ? ray_glob_match_compiled(&pc, s, slen)
                                   : ray_glob_match(s, slen, pat, pat_len);
                }
                out[i] = m ? 1 : 0;
            }
        }
        return result;
    }

    return ray_error("type", "like: expects string or symbol");
}

ray_t* ray_sym_name_fn(ray_t* x) {
    if (x->type == -RAY_I64) {
        if (x->i64 < 0 || !ray_sym_str(x->i64))
            return ray_error("domain", "sym-name: invalid sym ID");
        return ray_sym(x->i64);
    }
    if (x->type == RAY_I64) {
        int64_t n = x->len;
        const int64_t* data = (const int64_t*)ray_data(x);
        /* Validate all IDs first */
        for (int64_t i = 0; i < n; i++) {
            if (data[i] < 0 || !ray_sym_str(data[i]))
                return ray_error("domain", "sym-name: invalid sym ID in vector");
        }
        ray_t* out = ray_vec_new(RAY_SYM, n);
        if (RAY_IS_ERR(out)) return out;
        for (int64_t i = 0; i < n; i++) {
            out = ray_vec_append(out, &data[i]);
            if (RAY_IS_ERR(out)) return out;
        }
        return out;
    }
    /* Already sym (atom or vector), or empty I64/SYM vector — passthrough */
    if (x->type == -RAY_SYM || x->type == RAY_SYM ||
        ((x->type == RAY_I64 || x->type == RAY_SYM) && x->len == 0)) {
        ray_retain(x); return x;
    }
    return ray_error("type", "sym-name: expected i64 or i64 vector");
}
