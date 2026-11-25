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
    d->vals = ray_list_append(d->vals, v);
    ray_release(v);
    if (!d->vals || RAY_IS_ERR(d->vals)) d->oom = true;
}

static ray_t* insp_done(insp_dict_t* d) {
    if (d->oom) {
        if (d->keys && !RAY_IS_ERR(d->keys)) ray_release(d->keys);
        if (d->vals && !RAY_IS_ERR(d->vals)) ray_release(d->vals);
        return ray_error("oom", NULL);
    }
    return ray_dict_new(d->keys, d->vals);
}

static ray_t* insp_sym(const char* s) { return ray_sym(ray_sym_intern(s, strlen(s))); }

/* ---- lookup ---- */

/* The member `id` of the dict `d`, owned, or NULL.  Only symbol keys are
 * matched; a member of a typed value vector is its atom. */
static ray_t* insp_dict_member(ray_t* d, int64_t id) {
    ray_t* keys = ray_dict_keys(d);
    ray_t* vals = ray_dict_vals(d);
    if (!keys || !vals || keys->type != RAY_SYM) return NULL;
    for (int64_t i = 0; i < keys->len; i++) {
        if (ray_read_sym(ray_data(keys), i, RAY_SYM, keys->attrs) != id) continue;
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
    const char* kind = !v ? "missing" : lambda ? "lambda" : !builtin ? "value"
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
    if (own_str) {
        char nm[256];
        size_t l = ray_str_len(own_str);
        if (l < sizeof nm) {
            memcpy(nm, ray_str_ptr(own_str), l);
            nm[l] = '\0';
            usage = ray_sig_usage(nm);
        }
    }

    insp_dict_t d;
    insp_init(&d, 11);
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
    insp_put(&d, "builtin", own >= 0 ? ray_sym(own) : insp_sym(""));
    insp_put(&d, "restricted", ray_bool(builtin && (v->attrs & RAY_FN_RESTRICTED)));
    if (v) ray_release(v);
    return insp_done(&d);
}
