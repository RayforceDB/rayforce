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

#include "lang/inspect.h"
#include "lang/env.h"
#include "lang/eval.h"
#include "lang/sig.h"
#include "lang/format.h"
#include "table/sym.h"
#include "lang/nfo.h"
#include "core/crc32.h"
#include <stdio.h>
#include "table/table.h"
#include "store/enccol.h"
#include "ops/ops.h"
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- result dicts ---- */

typedef struct { ray_t* keys; ray_t* vals; bool oom; } insp_dict_t;

static void insp_init(insp_dict_t* d, int64_t n) {
    d->keys = ray_vec_new(RAY_SYM, n);
    d->vals = ray_list_new(n);
    d->oom = !d->keys || RAY_IS_ERR(d->keys) || !d->vals || RAY_IS_ERR(d->vals);
}

/* Adds `key: v`, `v` owned (released here). */
static void insp_put(insp_dict_t* d, const char* key, ray_t* v) {
    if (d->oom || !v || RAY_IS_ERR(v)) {
        if (v && !RAY_IS_ERR(v)) ray_release(v);
        else if (v) ray_error_free(v);
        d->oom = true;
        return;
    }
    ((int64_t*)ray_data(d->keys))[d->keys->len++] = ray_sym_intern(key, strlen(key));
    d->vals = ray_list_append_take(d->vals, v);
    ray_release(v);
    if (!d->vals || RAY_IS_ERR(d->vals)) d->oom = true;
}

static ray_t* insp_done(insp_dict_t* d) {
    if (d->oom) {
        /* each is ours: a value released, an error freed (#1864) */
        if (d->keys && RAY_IS_ERR(d->keys)) ray_error_free(d->keys); else ray_release(d->keys);
        if (d->vals && RAY_IS_ERR(d->vals)) ray_error_free(d->vals); else ray_release(d->vals);
        return ray_error("oom", NULL);
    }
    return ray_dict_new(d->keys, d->vals);
}

static ray_t* insp_sym(const char* s) { return ray_sym(ray_sym_intern(s, strlen(s))); }

/* ---- lookup ---- */

/* Cell `i` of a SYM vector as a runtime symbol id.  A vector from a stored
 * column holds positions in its file's domain, not runtime ids: its text is
 * looked up (-1 when the runtime table has no such symbol, so it names no
 * global and no member the caller asks for), or interned when `intern`. */
static int64_t insp_cell_sym(ray_t* vec, int64_t i, bool intern) {
    if (ray_sym_vec_domain(vec) == ray_sym_runtime_domain())
        return ray_read_sym(ray_data(vec), i, RAY_SYM, vec->attrs);
    ray_t* s = ray_sym_vec_cell(vec, i);
    if (!s) return -1;
    return intern ? ray_sym_intern(ray_str_ptr(s), ray_str_len(s))
                  : ray_sym_find(ray_str_ptr(s), ray_str_len(s));
}

/* The member `id` of the dict `d`, owned, or NULL.  Only symbol keys are
 * matched; a member of a typed value vector is its atom. */
static ray_t* insp_dict_member(ray_t* d, int64_t id) {
    ray_t* keys = ray_dict_keys(d);
    ray_t* vals = ray_dict_vals(d);
    if (!keys || !vals || keys->type != RAY_SYM) return NULL;
    for (int64_t i = 0; i < keys->len; i++) {
        if (insp_cell_sym(keys, i, false) != id) continue;
        if (vals->type == RAY_LIST) {
            ray_t* v = ((ray_t**)ray_data(vals))[i];
            if (v) ray_retain(v);
            return v;
        }
        ray_t* ix = ray_i64(i);
        if (!ix || RAY_IS_ERR(ix)) return NULL;
        ray_t* v = ray_at_fn(vals, ix);
        ray_release(ix);
        if (v && RAY_IS_ERR(v)) { ray_error_free(v); return NULL; }
        return v;
    }
    return NULL;
}

ray_t* ray_inspect_lookup(int64_t sym_id) {
    ray_t* v = ray_env_get_global(sym_id);
    if (v) { ray_retain(v); return v; }
    const int64_t* segs;
    int n = ray_sym_segs(sym_id, &segs);
    if (n < 2) return NULL;
    v = ray_env_get_global(segs[0]);
    if (!v) return NULL;
    ray_retain(v);
    for (int i = 1; i < n && v; i++) {
        ray_t* next = v->type == RAY_DICT ? insp_dict_member(v, segs[i]) : NULL;
        ray_release(v);
        v = next;
    }
    return v;
}

/* ---- .repl.signature (#947) ---- */

/* A namespace root: a dict bound under a one-segment dotted name (.sys,
 * .ipc, ...), a reserved name user code cannot bind. */
static bool insp_is_namespace(int64_t id, ray_t* v) {
    ray_t* s = v && v->type == RAY_DICT ? ray_sym_str(id) : NULL;
    if (!s) return false;
    const char* p = ray_str_ptr(s);
    size_t n = ray_str_len(s);
    return n > 1 && p[0] == '.' && !memchr(p + 1, '.', n - 1);
}

/* `true` and `false` are literals the parser reads, not bindings. */
static bool insp_is_literal(int64_t id) {
    ray_t* s = ray_sym_str(id);
    if (!s) return false;
    size_t n = ray_str_len(s);
    const char* p = ray_str_ptr(s);
    return (n == 4 && !memcmp(p, "true", 4)) || (n == 5 && !memcmp(p, "false", 5));
}

static bool insp_is_builtin(const ray_t* v) {
    return v && (v->type == RAY_UNARY || v->type == RAY_BINARY || v->type == RAY_VARY);
}

ray_t* ray_repl_signature_fn(ray_t* name) {
    if (!name || name->type != -RAY_SYM)
        return ray_error("type", ".repl.signature expects a symbol, got %s",
                         name ? ray_type_name(name->type) : "null");
    ray_t* v = ray_inspect_lookup(name->i64);
    bool lambda = v && v->type == RAY_LAMBDA;
    bool builtin = insp_is_builtin(v);
    const char* kind = !v ? (insp_is_literal(name->i64) ? "literal" : "missing")
                     : lambda ? "lambda"
                     : !builtin ? (insp_is_namespace(name->i64, v) ? "namespace" : "value")
                     : v->type == RAY_UNARY ? "unary" : v->type == RAY_BINARY ? "binary" : "vary";

    ray_t* params = NULL;
    if (lambda) {
        ray_t* p = LAMBDA_PARAMS(v);
        if (p && p->type == RAY_SYM) { ray_retain(p); params = p; }
    }
    if (!params) params = ray_vec_new(RAY_SYM, 0);

    int64_t own = builtin ? ray_env_builtin_own_sym(v) : -1;
    ray_t* own_str = own >= 0 ? ray_sym_str(own) : NULL;
    const char* usage = NULL;
    const char* summary = NULL;
    if (own_str) {
        char nm[256];
        size_t l = ray_str_len(own_str);
        if (l < sizeof nm) {
            memcpy(nm, ray_str_ptr(own_str), l);
            nm[l] = '\0';
            usage = ray_sig_usage(nm);
            summary = ray_sig_summary(nm);
        }
    }

    insp_dict_t d;
    insp_init(&d, 15);
    ray_retain(name);
    insp_put(&d, "name", name);
    insp_put(&d, "bound", ray_bool(v != NULL));
    insp_put(&d, "callable", ray_bool(lambda || builtin));
    insp_put(&d, "kind", insp_sym(kind));
    insp_put(&d, "special", ray_bool(builtin && (v->attrs & RAY_FN_SPECIAL_FORM)));
    insp_put(&d, "params", params);
    insp_put(&d, "arity", lambda && params && !RAY_IS_ERR(params) ? ray_i64(params->len)
                                                                  : ray_typed_null(-RAY_I64));
    insp_put(&d, "usage", usage ? ray_str(usage, strlen(usage)) : ray_str("", 0));
    insp_put(&d, "summary", summary ? ray_str(summary, strlen(summary)) : ray_str("", 0));
    insp_put(&d, "builtin", own >= 0 ? ray_sym(own) : insp_sym(""));
    /* restricted: refused under -U whatever its arguments; restricted_named:
     * refused under -U only when its target names a binding (#1878) */
    bool named = builtin && ray_fn_restricted_named(v);
    insp_put(&d, "restricted", ray_bool(builtin && (v->attrs & RAY_FN_RESTRICTED) && !named));
    insp_put(&d, "restricted_named", ray_bool(named));
    /* how it treats a collection, from what the engine records (#1744):
     * atomic, the evaluator maps it over vectors and lists itself; per
     * argument, whether that argument maps a vector element by element
     * (the row-shape record, at the arity it describes); aggregate, it
     * reduces.  Nothing for anything that is not a builtin. */
    insp_put(&d, "atomic", ray_bool(builtin && (v->attrs & RAY_FN_ATOMIC)));
    int64_t na = builtin ? ray_fn_row_arity(v) : 0;
    ray_t* ew = ray_vec_new(RAY_BOOL, na);
    if (ew && !RAY_IS_ERR(ew)) {
        for (int64_t i = 0; i < na; i++)
            ((uint8_t*)ray_data(ew))[i] = ray_fn_row_arg(v, na, i + 1) ? 1 : 0;
        ew->len = na;
    }
    insp_put(&d, "elementwise", ew);
    insp_put(&d, "aggregate", ray_bool(builtin && (v->attrs & RAY_FN_AGGR)));
    if (v) ray_release(v);
    return insp_done(&d);
}

/* ---- .repl.definition (#946) ---- */

/* The byte offset of line `line`, column `col` (both 0-based) in `src`, or
 * -1 past its end. */
static int64_t insp_offset(const char* src, int64_t len, int64_t line, int64_t col) {
    int64_t at = 0;
    for (int64_t l = 0; l < line; l++) {
        const char* nl = at < len ? memchr(src + at, '\n', (size_t)(len - at)) : NULL;
        if (!nl) return -1;
        at = (int64_t)(nl - src) + 1;
    }
    return at + col <= len ? at + col : -1;
}

static ray_t* insp_pos(int64_t line, int64_t col) {
    ray_t* p = ray_vec_new(RAY_I64, 2);
    if (!p || RAY_IS_ERR(p)) return p;
    ((int64_t*)ray_data(p))[0] = line;
    ((int64_t*)ray_data(p))[1] = col;
    p->len = 2;
    return p;
}

ray_t* ray_repl_definition_fn(ray_t* name) {
    if (!name || name->type != -RAY_SYM)
        return ray_error("type", ".repl.definition expects a symbol, got %s",
                         name ? ray_type_name(name->type) : "null");
    ray_t* v = ray_inspect_lookup(name->i64);
    bool lambda = v && v->type == RAY_LAMBDA;
    const char* kind = !v ? "missing" : lambda ? "lambda" : insp_is_builtin(v) ? "builtin" : "value";

    /* The lambda's extent, from what the parser recorded (0-based lines and
     * byte columns, inclusive end): its parameter list through its last
     * body expression.  It is a definition only when `(fn` stands right
     * before the parameters in the text; a lambda built at run time from
     * quoted pieces, `(eval (list 'fn ['x] 'x))`, has spans of those pieces
     * and none of a definition.  The extent then widens to that `(` and the
     * `)` after the body.  A coordinate the 16-bit span saturated is not a
     * position. */
    ray_t* nfo = lambda ? LAMBDA_NFO(v) : NULL;
    ray_t* file = nfo ? NFO_FILENAME(nfo) : NULL;
    ray_t* src = nfo ? NFO_SOURCE(nfo) : NULL;
    ray_span_t first = { .id = 0 }, last = { .id = 0 };
    if (nfo && src && file) {
        ray_t* body = LAMBDA_BODY(v);
        first = LAMBDA_PARAMS(v) ? ray_nfo_get(nfo, LAMBDA_PARAMS(v)) : first;
        if (body && body->type == RAY_LIST && body->len > 0) {
            ray_t** b = (ray_t**)ray_data(body);
            if (!first.id) first = ray_nfo_get(nfo, b[0]);
            last = ray_nfo_get(nfo, b[body->len - 1]);
        }
        if (!last.id) last = first;
    }
    bool available = first.id && last.id &&
                     first.start_line != UINT16_MAX && first.start_col != UINT16_MAX &&
                     last.end_line != UINT16_MAX && last.end_col != UINT16_MAX;
    int64_t sl = first.start_line, sc = first.start_col, el = last.end_line, ec = (int64_t)last.end_col + 1;
    int64_t from = -1, to = -1;
    if (available) {
        const char* sp = ray_str_ptr(src);
        int64_t slen = (int64_t)ray_str_len(src);
        from = insp_offset(sp, slen, sl, sc);
        to = insp_offset(sp, slen, el, ec);
        available = from >= 0 && to >= from;
        /* `(fn` and blanks right before the parameters */
        int64_t k = from - 1;
        while (available && k >= 0 && (sp[k] == ' ' || sp[k] == '\t' || sp[k] == '\n' || sp[k] == '\r')) k--;
        available = available && k >= 2 && sp[k] == 'n' && sp[k - 1] == 'f';
        k -= 2;
        while (available && k >= 0 && (sp[k] == ' ' || sp[k] == '\t' || sp[k] == '\n' || sp[k] == '\r')) k--;
        available = available && k >= 0 && sp[k] == '(';
        if (available) {
            int64_t e = to;
            while (e < slen && (sp[e] == ' ' || sp[e] == '\t' || sp[e] == '\n' || sp[e] == '\r')) e++;
            if (e < slen && sp[e] == ')') to = e + 1;
            from = k;
            /* the positions of the widened extent */
            sl = 0; sc = 0;
            for (int64_t i = 0; i < from; i++) { if (sp[i] == '\n') { sl++; sc = 0; } else sc++; }
            el = sl; ec = sc;
            for (int64_t i = from; i < to; i++) { if (sp[i] == '\n') { el++; ec = 0; } else ec++; }
        }
    }

    insp_dict_t d;
    insp_init(&d, 10);
    ray_retain(name);
    insp_put(&d, "name", name);
    insp_put(&d, "bound", ray_bool(v != NULL));
    insp_put(&d, "kind", insp_sym(kind));
    insp_put(&d, "available", ray_bool(available));
    if (available) {
        char hex[16];
        uint32_t crc = ray_crc32(0, ray_str_ptr(src), ray_str_len(src));
        snprintf(hex, sizeof hex, "%08x", crc);
        insp_put(&d, "file", ray_str(ray_str_ptr(file), ray_str_len(file)));
        insp_put(&d, "start", insp_pos(sl, sc));
        insp_put(&d, "end", insp_pos(el, ec));
        insp_put(&d, "text", ray_str(ray_str_ptr(src) + from, (size_t)(to - from)));
        insp_put(&d, "digest", ray_str(hex, strlen(hex)));
        insp_put(&d, "size", ray_i64((int64_t)ray_str_len(src)));
    } else {
        insp_put(&d, "file", ray_str("", 0));
        insp_put(&d, "start", ray_vec_new(RAY_I64, 0));
        insp_put(&d, "end", ray_vec_new(RAY_I64, 0));
        insp_put(&d, "text", ray_str("", 0));
        insp_put(&d, "digest", ray_str("", 0));
        insp_put(&d, "size", ray_typed_null(-RAY_I64));
    }
    if (v) ray_release(v);
    return insp_done(&d);
}

/* ---- .repl.inspect (#945) ---- */

#define INSP_LIMIT 1000

/* The type a value reads as, from its header alone: a parted or encoded
 * column its element type, a partition column the type of its key values.
 * False when that is not known without a read. */
static bool insp_logical_type(ray_t* v, int8_t* t) {
    if (!v || RAY_IS_ERR(v)) return false;
    if (v->type == RAY_ENCCOL) {
        ray_enccol_t* e = ray_enccol_of(v);
        if (!e) return false;
        *t = e->type;
        return true;
    }
    if (RAY_IS_PARTED(v->type)) { *t = (int8_t)RAY_PARTED_BASETYPE(v->type); return true; }
    if (v->type == RAY_MAPCOMMON) {
        ray_t* kv = ((ray_t**)ray_data(v))[0];
        if (!kv || RAY_IS_ERR(kv)) return false;
        *t = kv->type;
        return true;
    }
    *t = v->type;
    return true;
}

static int64_t insp_type_id(ray_t* v) {
    if (v && !RAY_IS_ERR(v) && RAY_IS_NULL(v)) return ray_sym_intern("null", 4);
    int8_t t;
    if (!insp_logical_type(v, &t)) return ray_sym_intern("", 0);
    const char* nm = ray_type_name(t);
    return ray_sym_intern(nm, strlen(nm));
}

/* Rows of a column held in its header (a plain vector, an encoded column, a
 * partition column's in-memory counts), or -1 when counting them would
 * read a plain parted column's segments. */
static int64_t insp_col_rows(ray_t* c) {
    if (!c || RAY_IS_ERR(c) || RAY_IS_PARTED(c->type)) return -1;
    return ray_table_col_nrows(c);
}

static ray_t* insp_len(ray_t* v) {
    int64_t n = -1;
    if (v->type == RAY_DICT) {
        ray_t* k = ray_dict_keys(v);
        n = k ? k->len : -1;
    } else if (v->type == RAY_TABLE) {
        int64_t nc = ray_table_ncols(v);
        if (nc == 0) n = 0;
        for (int64_t i = 0; i < nc && n < 0; i++) n = insp_col_rows(ray_table_slot_idx(v, i));
    } else if (v->type == RAY_ENCCOL || v->type == RAY_MAPCOMMON || RAY_IS_PARTED(v->type)) {
        n = insp_col_rows(v);
    } else if (v->type >= 0 && v->type != RAY_LAMBDA && !insp_is_builtin(v) && !RAY_IS_NULL(v)) {
        n = v->len;
    }
    return n >= 0 ? ray_i64(n) : ray_typed_null(-RAY_I64);
}

static const char* insp_kind(ray_t* v) {
    if (!v) return "missing";
    if (RAY_IS_NULL(v) || v->type < 0) return "atom";
    switch (v->type) {
    case RAY_DICT:   return "dict";
    case RAY_TABLE:  return "table";
    case RAY_LAMBDA: return "lambda";
    case RAY_LIST:   return "list";
    default: break;
    }
    if (insp_is_builtin(v)) return "builtin";
    if (ray_is_vec(v) || v->type == RAY_ENCCOL || v->type == RAY_MAPCOMMON || RAY_IS_PARTED(v->type))
        return "vector";
    return "other";
}

/* Follows `path` (a symbol, or a symbol vector [root member ...]) through
 * dict members and, last, one table column, read from the slot as stored. */
static ray_t* insp_follow(ray_t* path) {
    if (path->type == -RAY_SYM) return ray_inspect_lookup(path->i64);
    int64_t n = path->len;
    int64_t root = insp_cell_sym(path, 0, false);
    ray_t* v = root >= 0 ? ray_inspect_lookup(root) : NULL;
    for (int64_t i = 1; i < n && v; i++) {
        int64_t id = insp_cell_sym(path, i, false);
        ray_t* next = NULL;
        if (v->type == RAY_DICT) next = insp_dict_member(v, id);
        else if (v->type == RAY_TABLE && i == n - 1) {
            next = ray_table_slot(v, id);
            if (next) ray_retain(next);
        }
        ray_release(v);
        v = next;
    }
    return v;
}

typedef struct { int64_t id; int64_t type; const char* s; size_t n; } insp_member_t;

static int insp_member_cmp(const void* a, const void* b) {
    const insp_member_t* x = a;
    const insp_member_t* y = b;
    size_t m = x->n < y->n ? x->n : y->n;
    int c = memcmp(x->s, y->s, m);
    return c ? c : (x->n > y->n) - (x->n < y->n);
}

/* The members of a dict (symbol keys) or the columns of a table whose names
 * start with `pfx`, sorted by name, at most `limit` of them: their names
 * into *keys, their types into *types.  One pass over the members and a
 * sort of the matches; no value is read past its header. */
static bool insp_members(ray_t* v, const char* pfx, size_t plen, int64_t limit,
                         ray_t** keys, ray_t** types, bool* truncated) {
    *keys = NULL; *types = NULL; *truncated = false;
    bool table = v && v->type == RAY_TABLE;
    ray_t* dk = v && v->type == RAY_DICT ? ray_dict_keys(v) : NULL;
    ray_t* dv = dk ? ray_dict_vals(v) : NULL;
    int64_t n = table ? ray_table_ncols(v) : dk && dv && dk->type == RAY_SYM ? dk->len : 0;

    ray_t* scratch = ray_vec_new(RAY_U8, n * (int64_t)sizeof(insp_member_t) + 1);
    if (!scratch || RAY_IS_ERR(scratch)) return false;
    insp_member_t* m = (insp_member_t*)ray_data(scratch);
    int64_t hits = 0;
    for (int64_t i = 0; i < n; i++) {
        /* a stored key column's names are its domain's text; such a name
         * is interned only if it is returned */
        bool cell = !table && ray_sym_vec_domain(dk) != ray_sym_runtime_domain();
        int64_t id = table ? ray_table_col_name(v, i)
                   : cell ? -1 : ray_read_sym(ray_data(dk), i, RAY_SYM, dk->attrs);
        ray_t* nm = cell ? ray_sym_vec_cell(dk, i) : ray_sym_str(id);
        const char* s = nm ? ray_str_ptr(nm) : "";
        size_t len = nm ? ray_str_len(nm) : 0;
        if (len < plen || memcmp(s, pfx, plen) != 0) continue;
        int64_t ty;
        if (table) ty = insp_type_id(ray_table_slot_idx(v, i));
        else if (dv->type == RAY_LIST) ty = insp_type_id(((ray_t**)ray_data(dv))[i]);
        else {
            const char* tn = ray_type_name((int8_t)-dv->type);
            ty = ray_sym_intern(tn, strlen(tn));
        }
        m[hits++] = (insp_member_t){ id, ty, s, len };
    }
    qsort(m, (size_t)hits, sizeof *m, insp_member_cmp);
    int64_t out = hits < limit ? hits : limit;
    *truncated = hits > out;
    *keys = ray_vec_new(RAY_SYM, out);
    *types = ray_vec_new(RAY_SYM, out);
    bool ok = *keys && !RAY_IS_ERR(*keys) && *types && !RAY_IS_ERR(*types);
    if (ok) {
        for (int64_t i = 0; i < out; i++) {
            ((int64_t*)ray_data(*keys))[i] = m[i].id >= 0 ? m[i].id : ray_sym_intern(m[i].s, m[i].n);
            ((int64_t*)ray_data(*types))[i] = m[i].type;
        }
        (*keys)->len = out;
        (*types)->len = out;
    }
    ray_release(scratch);
    return ok;
}

/* Reads {prefix: str, limit: integer} into the prefix string atom (owned:
 * a member of a typed value vector is a fresh atom, so the caller keeps it
 * until the prefix is no longer read and then releases it) and the limit;
 * NULL, or an error with *pfx released. */
static ray_t* insp_opts(ray_t* opts, ray_t** pfx, int64_t* limit) {
    if (opts->type != RAY_DICT)
        return ray_error("type", ".repl.inspect options must be a dict, got %s", ray_type_name(opts->type));
    ray_t* k = ray_dict_keys(opts);
    if (k && k->len == 0) return NULL;   /* {}: no options */
    if (!k || k->type != RAY_SYM)
        return ray_error("type", ".repl.inspect options must have symbol keys");
    int64_t prefix_id = ray_sym_intern("prefix", 6), limit_id = ray_sym_intern("limit", 5);
    for (int64_t i = 0; i < k->len; i++) {
        int64_t id = insp_cell_sym(k, i, false);
        if (id != prefix_id && id != limit_id) {
            ray_t* nm = ray_sym_vec_cell(k, i);
            return ray_error("domain", ".repl.inspect: unknown option '%.*s' (prefix, limit)",
                             nm ? (int)ray_str_len(nm) : 0, nm ? ray_str_ptr(nm) : "");
        }
        ray_t* v = insp_dict_member(opts, id);
        ray_t* err = NULL;
        if (id == prefix_id) {
            if (!v || v->type != -RAY_STR)
                err = ray_error("type", ".repl.inspect: prefix must be a string");
            else {
                if (*pfx) ray_release(*pfx);
                *pfx = v;
                v = NULL;
            }
        } else {
            int64_t lim = 0;
            bool integer = v && !RAY_IS_NULL(v) && !RAY_IS_ERR(v);
            if (integer) {
                switch (v->type) {
                case -RAY_I64: lim = v->i64; break;
                case -RAY_I32: lim = v->i32; break;
                case -RAY_I16: lim = v->i16; break;
                case -RAY_U8:  lim = v->u8;  break;
                default: integer = false; break;
                }
            }
            if (!integer)
                err = ray_error("type", ".repl.inspect: limit must be an integer");
            else if (RAY_ATOM_IS_NULL(v))
                err = ray_error("type", ".repl.inspect: limit must not be null");
            else if (lim < 0)
                err = ray_error("domain", ".repl.inspect: limit must not be negative, got %lld",
                                (long long)lim);
            else *limit = lim;
        }
        if (v) ray_release(v);
        if (err) {
            if (*pfx) { ray_release(*pfx); *pfx = NULL; }
            return err;
        }
    }
    return NULL;
}

ray_t* ray_repl_inspect_fn(ray_t** args, int64_t n) {
    if (n < 1 || n > 2)
        return ray_error("arity", ".repl.inspect expects 1 or 2 arguments, got %lld", (long long)n);
    ray_t* path = args[0];
    if (!path || !(path->type == -RAY_SYM || (path->type == RAY_SYM && path->len > 0)))
        return ray_error("type", ".repl.inspect expects a symbol or a non-empty symbol vector, got %s",
                         path ? ray_type_name(path->type) : "null");
    ray_t* pfx = NULL;
    int64_t limit = INSP_LIMIT;
    if (n == 2) {
        ray_t* err = insp_opts(args[1], &pfx, &limit);
        if (err) return err;
    }

    ray_t* v = insp_follow(path);
    ray_t *keys, *types;
    bool truncated;
    bool listed = insp_members(v, pfx ? ray_str_ptr(pfx) : "", pfx ? ray_str_len(pfx) : 0,
                               limit, &keys, &types, &truncated);
    if (pfx) ray_release(pfx);
    if (!listed) {
        if (keys && !RAY_IS_ERR(keys)) ray_release(keys);
        if (types && !RAY_IS_ERR(types)) ray_release(types);
        if (v) ray_release(v);
        return ray_error("oom", NULL);
    }

    insp_dict_t d;
    insp_init(&d, 8);
    ray_retain(path);
    insp_put(&d, "path", path);
    insp_put(&d, "bound", ray_bool(v != NULL));
    insp_put(&d, "type", ray_sym(v ? insp_type_id(v) : ray_sym_intern("", 0)));
    insp_put(&d, "kind", insp_sym(insp_kind(v)));
    insp_put(&d, "len", v ? insp_len(v) : ray_typed_null(-RAY_I64));
    insp_put(&d, "keys", keys);
    insp_put(&d, "types", types);
    insp_put(&d, "truncated", ray_bool(truncated));
    if (v) ray_release(v);
    return insp_done(&d);
}
