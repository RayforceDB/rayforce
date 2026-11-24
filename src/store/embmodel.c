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

#include "store/embmodel.h"
#include "io/json.h"
#include "lang/eval.h"
#include "lang/internal.h"
#include "mem/sys.h"
#include "ops/internal.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

/* The tokenizer knows nothing of the engine; this is where it gets its memory. */
#define emb_alloc(n) ray_sys_alloc(n)
#define emb_free(p)  do { void* p_ = (p); if (p_) ray_sys_free(p_); } while (0)
#include "store/embtok.h"

struct ray_embmodel {
    _Atomic(int64_t) rc;
    emb_tok_t* tok;
    int64_t dim, layers, heads, inner, max_tokens;
    char kind[32];
};

struct ray_embwork {
    emb_scratch_t scratch;
};

void ray_embmodel_retain(ray_embmodel_t* m) {
    if (m) atomic_fetch_add_explicit(&m->rc, 1, memory_order_relaxed);
}

void ray_embmodel_release(ray_embmodel_t* m) {
    if (!m) return;
    if (atomic_fetch_sub_explicit(&m->rc, 1, memory_order_acq_rel) != 1) return;
    emb_tok_free(m->tok);
    ray_sys_free(m);
}

void ray_embmodel_info(const ray_embmodel_t* m, ray_embmodel_info_t* out) {
    out->vocab = m->tok->n; out->dim = m->dim; out->layers = m->layers; out->heads = m->heads;
    out->inner = m->inner; out->max_tokens = m->max_tokens; out->kind = m->kind; out->tokenizer = "unigram";
}

ray_embwork_t* ray_embwork_new(const ray_embmodel_t* m) {
    (void)m;
    ray_embwork_t* w = (ray_embwork_t*)ray_sys_alloc(sizeof *w);   /* zero-filled */
    return w;
}

void ray_embwork_free(ray_embwork_t* w) {
    if (!w) return;
    emb_scratch_free(&w->scratch);
    ray_sys_free(w);
}

int64_t ray_embmodel_tokens(const ray_embmodel_t* m, ray_embwork_t* w,
                            const char* text, size_t len, const int32_t** ids) {
    int64_t n = emb_tok_encode(m->tok, &w->scratch, text, len);
    *ids = w->scratch.ids;
    return n;
}

/* ---- reading the model's files ------------------------------------------ */

/* The file `name` of the directory, as a string; an error object when it
 * cannot be read. */
static ray_t* emb_file(const char* dir, const char* name) {
    char path[1200];
    int k = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (k < 0 || (size_t)k >= sizeof path) return ray_error("domain", ".emb.model.get: the path of %s is too long", name);
    ray_t* p = ray_str(path, (size_t)k);
    if (!p || RAY_IS_ERR(p)) return p ? p : ray_error("oom", NULL);
    ray_t* s = ray_read_file_fn(p);
    ray_release(p);
    if (!s) return ray_error("io", ".emb.model.get: cannot read %s", path);
    if (RAY_IS_ERR(s)) { ray_release(s); return ray_error("io", ".emb.model.get: cannot read %s", path); }
    return s;
}

/* The caps of the JSON reader, raised to what a tokenizer's file needs: a
 * vocabulary of some hundred thousand entries in a document of tens of MB. */
static ray_t* emb_limits(void) {
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 3);
    if (!keys || RAY_IS_ERR(keys)) return keys ? keys : ray_error("oom", NULL);
    ray_t* vals = ray_list_new(3);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals ? vals : ray_error("oom", NULL); }
    struct { const char* name; int64_t v; } rows[] = {
        { "input", (int64_t)1 << 30 }, { "nodes", (int64_t)1 << 24 }, { "alloc", (int64_t)1 << 31 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        int64_t s = sym_intern_safe(rows[i].name, strlen(rows[i].name));
        keys = ray_vec_append(keys, &s);
        ray_t* v = make_i64(rows[i].v);
        vals = ray_list_append(vals, v);
        ray_release(v);
    }
    return ray_dict_new(keys, vals);
}

/* One field of a JSON document by its dotted path: an owned value, NULL when
 * the path is not there, an error object when the document is not JSON. */
static ray_t* emb_field(ray_t* doc, const char* path, ray_t* limits) {
    ray_t* p = ray_str(path, strlen(path));
    if (!p || RAY_IS_ERR(p)) return p ? p : ray_error("oom", NULL);
    ray_t* args[3] = { doc, p, limits };
    ray_t* v = ray_json_get_fn(args, 3);
    ray_release(p);
    if (v && !RAY_IS_ERR(v) && RAY_IS_NULL(v)) { ray_release(v); return NULL; }
    return v;
}

static bool emb_is_str(ray_t* v) { return v && !RAY_IS_ERR(v) && v->type == -RAY_STR; }

/* A whole number held in a field, or `miss` when the field is not one. */
static int64_t emb_int(ray_t* doc, const char* path, ray_t* limits, int64_t miss) {
    ray_t* v = emb_field(doc, path, limits);
    int64_t out = miss;
    if (v && !RAY_IS_ERR(v)) {
        if (v->type == -RAY_I64) out = v->i64;
        else if (v->type == -RAY_F64 && v->f64 == (double)(int64_t)v->f64) out = (int64_t)v->f64;
    }
    if (v) ray_release(v);
    return out;
}

/* Decodes base64 (RFC 4648, padded or not) into a fresh block; *out_len its
 * length.  NULL when the text is not base64 or memory runs out. */
static uint8_t* emb_base64(const char* s, size_t n, size_t* out_len) {
    static const int8_t T[256] = {
        ['A'] = 1, ['B'] = 2, ['C'] = 3, ['D'] = 4, ['E'] = 5, ['F'] = 6, ['G'] = 7, ['H'] = 8, ['I'] = 9, ['J'] = 10, ['K'] = 11, ['L'] = 12, ['M'] = 13,
        ['N'] = 14, ['O'] = 15, ['P'] = 16, ['Q'] = 17, ['R'] = 18, ['S'] = 19, ['T'] = 20, ['U'] = 21, ['V'] = 22, ['W'] = 23, ['X'] = 24, ['Y'] = 25, ['Z'] = 26,
        ['a'] = 27, ['b'] = 28, ['c'] = 29, ['d'] = 30, ['e'] = 31, ['f'] = 32, ['g'] = 33, ['h'] = 34, ['i'] = 35, ['j'] = 36, ['k'] = 37, ['l'] = 38, ['m'] = 39,
        ['n'] = 40, ['o'] = 41, ['p'] = 42, ['q'] = 43, ['r'] = 44, ['s'] = 45, ['t'] = 46, ['u'] = 47, ['v'] = 48, ['w'] = 49, ['x'] = 50, ['y'] = 51, ['z'] = 52,
        ['0'] = 53, ['1'] = 54, ['2'] = 55, ['3'] = 56, ['4'] = 57, ['5'] = 58, ['6'] = 59, ['7'] = 60, ['8'] = 61, ['9'] = 62, ['+'] = 63, ['/'] = 64,
    };
    while (n > 0 && s[n - 1] == '=') n--;
    uint8_t* out = (uint8_t*)ray_sys_alloc(n / 4 * 3 + 4);
    if (!out) return NULL;
    size_t w = 0; uint32_t acc = 0; int bits = 0;
    for (size_t i = 0; i < n; i++) {
        int8_t v = T[(uint8_t)s[i]];
        if (v == 0) { ray_sys_free(out); return NULL; }
        acc = (acc << 6) | (uint32_t)(v - 1); bits += 6;
        if (bits >= 8) { bits -= 8; out[w++] = (uint8_t)(acc >> bits); }
    }
    *out_len = w;
    return out;
}

/* The tokenizer described by tokenizer.json; NULL with *err set otherwise. */
static emb_tok_t* emb_read_tokenizer(const char* dir, ray_t* limits, ray_t** err) {
    emb_tok_t* tok = NULL;
    ray_t *doc = NULL, *kind = NULL, *vocab = NULL, *map = NULL;
    uint8_t* charsmap = NULL; size_t charsmap_len = 0;
    const char** toks = NULL; uint32_t* lens = NULL; double* scores = NULL;
#define EMB_FAIL(...) do { *err = ray_error(__VA_ARGS__); goto done; } while (0)
    doc = emb_file(dir, "tokenizer.json");
    if (RAY_IS_ERR(doc)) { *err = doc; doc = NULL; goto done; }
    kind = emb_field(doc, "model.type", limits);
    if (kind && RAY_IS_ERR(kind)) { *err = kind; kind = NULL; goto done; }
    if (!emb_is_str(kind) || ray_str_len(kind) != 7 || memcmp(ray_str_ptr(kind), "Unigram", 7) != 0)
        EMB_FAIL("domain", ".emb.model.get: tokenizer.json holds a tokenizer of another kind than Unigram, which is the one kind read");
    /* The table of replacements is the normalizer itself, or one of a short sequence of them. */
    static const char* const at[] = {
        "normalizer.precompiled_charsmap", "normalizer.normalizers.0.precompiled_charsmap",
        "normalizer.normalizers.1.precompiled_charsmap", "normalizer.normalizers.2.precompiled_charsmap",
    };
    for (size_t i = 0; i < sizeof at / sizeof at[0] && !map; i++) {
        map = emb_field(doc, at[i], limits);
        if (map && RAY_IS_ERR(map)) { ray_release(map); map = NULL; }
        if (map && !emb_is_str(map)) { ray_release(map); map = NULL; }
    }
    if (map) {
        charsmap = emb_base64(ray_str_ptr(map), ray_str_len(map), &charsmap_len);
        if (!charsmap) EMB_FAIL("domain", ".emb.model.get: the tokenizer's table of replacements is not base64");
    }
    vocab = emb_field(doc, "model.vocab", limits);
    if (vocab && RAY_IS_ERR(vocab)) { *err = vocab; vocab = NULL; goto done; }
    if (!vocab || vocab->type != RAY_LIST || vocab->len < 4 || vocab->len > INT32_MAX / 2)
        EMB_FAIL("domain", ".emb.model.get: tokenizer.json has no vocabulary at model.vocab");
    int32_t n = (int32_t)vocab->len;
    toks = (const char**)ray_sys_alloc((size_t)n * sizeof *toks);
    lens = (uint32_t*)ray_sys_alloc((size_t)n * sizeof *lens);
    scores = (double*)ray_sys_alloc((size_t)n * sizeof *scores);
    if (!toks || !lens || !scores) EMB_FAIL("oom", NULL);
    int32_t bos = -1, eos = -1;
    for (int32_t i = 0; i < n; i++) {
        ray_t* e = ray_list_get(vocab, i);
        ray_t* t = e && e->type == RAY_LIST && e->len == 2 ? ray_list_get(e, 0) : NULL;
        ray_t* s = t ? ray_list_get(e, 1) : NULL;
        if (!emb_is_str(t) || !s || (s->type != -RAY_F64 && s->type != -RAY_I64))
            EMB_FAIL("domain", ".emb.model.get: entry %d of the vocabulary is not a token and its score", (int)i);
        toks[i] = ray_str_ptr(t);
        lens[i] = (uint32_t)ray_str_len(t);
        scores[i] = s->type == -RAY_F64 ? s->f64 : (double)s->i64;
        if (bos < 0 && lens[i] == 3 && memcmp(toks[i], "<s>", 3) == 0) bos = i;
        if (eos < 0 && lens[i] == 4 && memcmp(toks[i], "</s>", 4) == 0) eos = i;
    }
    int64_t unk = emb_int(doc, "model.unk_id", limits, -1);
    if (unk < 0 || unk >= n || bos < 0 || eos < 0)
        EMB_FAIL("domain", ".emb.model.get: the vocabulary lacks the token for the unknown, for the beginning (<s>) or for the end (</s>)");
    tok = emb_tok_new(charsmap, charsmap_len, toks, lens, scores, n, (int32_t)unk, bos, eos);
    if (!tok) EMB_FAIL("domain", ".emb.model.get: the tokenizer's tables could not be built: not in form, or out of memory");
done:
#undef EMB_FAIL
    if (toks) ray_sys_free((void*)toks);
    if (lens) ray_sys_free(lens);
    if (scores) ray_sys_free(scores);
    if (charsmap) ray_sys_free(charsmap);
    if (map) ray_release(map);
    if (vocab) ray_release(vocab);
    if (kind) ray_release(kind);
    if (doc) ray_release(doc);
    return tok;
}

ray_embmodel_t* ray_embmodel_open(const char* dir, ray_t** err) {
    *err = NULL;
    ray_t* limits = emb_limits();
    if (!limits || RAY_IS_ERR(limits)) { *err = limits ? limits : ray_error("oom", NULL); return NULL; }
    ray_embmodel_t* m = (ray_embmodel_t*)ray_sys_alloc(sizeof *m);
    if (!m) { ray_release(limits); *err = ray_error("oom", NULL); return NULL; }
    atomic_store(&m->rc, 1);
    ray_t* cfg = emb_file(dir, "config.json");
    if (RAY_IS_ERR(cfg)) { *err = cfg; cfg = NULL; goto fail; }
    {
        ray_t* kind = emb_field(cfg, "model_type", limits);
        if (emb_is_str(kind)) {
            size_t k = ray_str_len(kind) < sizeof m->kind - 1 ? ray_str_len(kind) : sizeof m->kind - 1;
            memcpy(m->kind, ray_str_ptr(kind), k);
        }
        if (kind) ray_release(kind);
    }
    m->dim = emb_int(cfg, "hidden_size", limits, 0);
    m->layers = emb_int(cfg, "num_hidden_layers", limits, 0);
    m->heads = emb_int(cfg, "num_attention_heads", limits, 0);
    m->inner = emb_int(cfg, "intermediate_size", limits, 0);
    m->max_tokens = emb_int(cfg, "max_position_embeddings", limits, 0);
    if (m->dim <= 0 || m->layers <= 0 || m->heads <= 0 || m->inner <= 0 || m->max_tokens < 3 || m->dim % m->heads != 0) {
        *err = ray_error("domain", ".emb.model.get: config.json does not describe an encoder: hidden_size, num_hidden_layers, num_attention_heads, intermediate_size and max_position_embeddings are needed");
        goto fail;
    }
    m->tok = emb_read_tokenizer(dir, limits, err);
    if (!m->tok) goto fail;
    ray_release(cfg);
    ray_release(limits);
    return m;
fail:
    if (cfg) ray_release(cfg);
    ray_release(limits);
    ray_embmodel_release(m);
    if (!*err) *err = ray_error("io", ".emb.model.get: the model could not be opened");
    return NULL;
}
