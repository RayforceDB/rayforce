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
#include "core/platform.h"
#include <math.h>

/* One layer of the encoder.  A dense weight is kept as the kernel wants it:
 * [in x out], rows in one block, with the pointers to its rows beside it. */
typedef struct {
    float *qkv, *qkv_b;  const float** qkv_r;   /* H x 3H: query, key and value side by side */
    float *ao,  *ao_b;   const float** ao_r;    /* H x H */
    float *in,  *in_b;   const float** in_r;    /* H x I */
    float *out, *out_b;  const float** out_r;   /* I x H */
    float *ln1_w, *ln1_b, *ln2_w, *ln2_b;       /* H each */
} emb_layer_t;

struct ray_embmodel {
    _Atomic(int64_t) rc;
    emb_tok_t* tok;
    int64_t dim, layers, heads, inner, max_tokens;
    char kind[32];
    /* the weights */
    void* map; size_t map_len;                 /* model.safetensors, mapped */
    const float *word, *pos, *type;            /* the three tables, in the mapping */
    int64_t words;                             /* rows of `word` */
    float *eln_w, *eln_b;                      /* the embeddings' layer norm */
    float eps;
    emb_layer_t* layer;
    void** owned; int64_t n_owned;             /* every block the weights own */
};

struct ray_embwork {
    emb_scratch_t scratch;
    /* the forward pass, sized for `cap` tokens */
    int64_t cap;
    float *x, *qkv, *kt, *sc, *ch, *ctx, *mid;
    const float** rows;   /* pointers to the rows of whatever is on the left */
    const float** krows;  /* and to the rows of the transposed keys of one head */
    const float** vrows;  /* and to one head's values */
};

void ray_embmodel_retain(ray_embmodel_t* m) {
    if (m) atomic_fetch_add_explicit(&m->rc, 1, memory_order_relaxed);
}

void ray_embmodel_release(ray_embmodel_t* m) {
    if (!m) return;
    if (atomic_fetch_sub_explicit(&m->rc, 1, memory_order_acq_rel) != 1) return;
    emb_tok_free(m->tok);
    for (int64_t i = 0; i < m->n_owned; i++) ray_sys_free(m->owned[i]);
    if (m->owned) ray_sys_free(m->owned);
    if (m->layer) ray_sys_free(m->layer);
    if (m->map) ray_vm_unmap_file(m->map, m->map_len);
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

static void emb_work_drop(ray_embwork_t* w) {
    emb_free(w->x); emb_free(w->qkv); emb_free(w->kt); emb_free(w->sc); emb_free(w->ch);
    emb_free(w->ctx); emb_free(w->mid); emb_free((void*)w->rows); emb_free((void*)w->krows); emb_free((void*)w->vrows);
    w->x = w->qkv = w->kt = w->sc = w->ch = w->ctx = w->mid = NULL;
    w->rows = w->krows = w->vrows = NULL;
    w->cap = 0;
}

void ray_embwork_free(ray_embwork_t* w) {
    if (!w) return;
    emb_scratch_free(&w->scratch);
    emb_work_drop(w);
    ray_sys_free(w);
}

/* Room for a text of t tokens. */
static bool emb_work_fit(const ray_embmodel_t* m, ray_embwork_t* w, int64_t t) {
    if (t <= w->cap) return true;
    emb_work_drop(w);
    size_t T = (size_t)t, H = (size_t)m->dim, I = (size_t)m->inner, dh = H / (size_t)m->heads;
    w->x   = (float*)ray_sys_alloc(T * H * sizeof(float));
    w->qkv = (float*)ray_sys_alloc(T * 3 * H * sizeof(float));
    w->kt  = (float*)ray_sys_alloc(dh * T * sizeof(float));
    w->sc  = (float*)ray_sys_alloc(T * T * sizeof(float));
    w->ch  = (float*)ray_sys_alloc(T * dh * sizeof(float));
    w->ctx = (float*)ray_sys_alloc(T * H * sizeof(float));
    w->mid = (float*)ray_sys_alloc(T * I * sizeof(float));
    w->rows  = (const float**)ray_sys_alloc(T * sizeof(float*));
    w->krows = (const float**)ray_sys_alloc(dh * sizeof(float*));
    w->vrows = (const float**)ray_sys_alloc(T * sizeof(float*));
    if (!w->x || !w->qkv || !w->kt || !w->sc || !w->ch || !w->ctx || !w->mid || !w->rows || !w->krows || !w->vrows) { emb_work_drop(w); return false; }
    w->cap = t;
    return true;
}

int64_t ray_embmodel_tokens(const ray_embmodel_t* m, ray_embwork_t* w,
                            const char* text, size_t len, const int32_t** ids) {
    int64_t n = emb_tok_encode(m->tok, &w->scratch, text, len);
    *ids = w->scratch.ids;
    return n;
}


/* ---- the forward pass ---------------------------------------------------- */

/* exp for the softmax: x <= 0 there.  Range reduction by powers of two and a
 * polynomial on the remainder, written so that the loop it sits in turns into
 * SIMD; within 2e-7 of the library's, relatively. */
static inline float emb_exp(float x) {
    x = x < -87.0f ? -87.0f : x;
    float n = __builtin_floorf(x * 1.44269504088896341f + 0.5f);
    float r = x - n * 0.693359375f + n * 2.12194440e-4f;
    float p = 1.9875691500e-4f;
    p = p * r + 1.3981999507e-3f;
    p = p * r + 8.3334519073e-3f;
    p = p * r + 4.1665795894e-2f;
    p = p * r + 1.6666665459e-1f;
    p = p * r + 5.0000001201e-1f;
    float e = p * r * r + r + 1.0f;
    int32_t bits = ((int32_t)n + 127) << 23;
    float scale;
    memcpy(&scale, &bits, sizeof scale);
    return e * scale;
}

/* gelu as the models are trained with it, x/2 (1 + erf(x/sqrt 2)), with erf
 * by a rational form that needs no exp (Abramowitz and Stegun 7.1.28, within
 * 3e-7): it too is written to turn into SIMD. */
static inline float emb_gelu(float x) {
    float z = __builtin_fabsf(x) * 0.70710678118654752f;
    float d = 1.0f + z * (0.0705230784f + z * (0.0422820123f + z * (0.0092705272f
              + z * (0.0001520143f + z * (0.0002765672f + z * 0.0000430638f)))));
    d *= d; d *= d; d *= d; d *= d;
    float erf = 1.0f - 1.0f / d;
    return 0.5f * x * (1.0f + __builtin_copysignf(erf, x));
}

/* y = (x - mean) / sqrt(var + eps) * w + b over each row of x, in place. */
static void emb_layer_norm(float* x, int64_t t, int64_t h, const float* w, const float* b, float eps) {
    for (int64_t i = 0; i < t; i++) {
        float* r = x + i * h;
        float mean = 0.0f;
        for (int64_t j = 0; j < h; j++) mean += r[j];
        mean /= (float)h;
        float var = 0.0f;
        for (int64_t j = 0; j < h; j++) { float d = r[j] - mean; var += d * d; }
        float inv = 1.0f / __builtin_sqrtf(var / (float)h + eps);
        for (int64_t j = 0; j < h; j++) r[j] = (r[j] - mean) * inv * w[j] + b[j];
    }
}

/* c[t x cols] = bias under every row, then += a * b through the kernel the
 * matrix product has (ops/linalg.c). */
static void emb_dense(const float* a, int64_t t, int64_t k, const float* const* b_rows, const float* bias,
                      float* c, int64_t cols, const float** rows) {
    for (int64_t i = 0; i < t; i++) { rows[i] = a + i * k; memcpy(c + i * cols, bias, (size_t)cols * sizeof(float)); }
    ray_lin_gemm_acc_f32(rows, b_rows, c, t, k, cols);
}

/* The vector of the tokens `ids` (t of them, t <= w->cap): the encoder's
 * last layer averaged over the tokens, scaled to length one. */
static void emb_forward(const ray_embmodel_t* m, ray_embwork_t* w, const int32_t* ids, int64_t t, float* out) {
    const int64_t H = m->dim, I = m->inner, nh = m->heads, dh = H / nh;
    float* x = w->x;
    for (int64_t i = 0; i < t; i++) {
        int64_t id = ids[i] >= 0 && ids[i] < m->words ? ids[i] : m->tok->unk;
        const float* e = m->word + id * H;
        const float* p = m->pos + i * H;
        float* r = x + i * H;
        for (int64_t j = 0; j < H; j++) r[j] = e[j] + p[j] + m->type[j];
    }
    emb_layer_norm(x, t, H, m->eln_w, m->eln_b, m->eps);
    const float scale = 1.0f / __builtin_sqrtf((float)dh);
    for (int64_t l = 0; l < m->layers; l++) {
        const emb_layer_t* L = &m->layer[l];
        /* query, key and value in one product */
        emb_dense(x, t, H, L->qkv_r, L->qkv_b, w->qkv, 3 * H, w->rows);
        for (int64_t h = 0; h < nh; h++) {
            const float* q0 = w->qkv + h * dh;
            const float* k0 = w->qkv + H + h * dh;
            const float* v0 = w->qkv + 2 * H + h * dh;
            /* the keys of this head turned on their side: dh rows of t */
            for (int64_t d = 0; d < dh; d++) {
                float* kr = w->kt + d * t;
                w->krows[d] = kr;
                for (int64_t i = 0; i < t; i++) kr[i] = k0[i * 3 * H + d] * scale;
            }
            for (int64_t i = 0; i < t; i++) w->rows[i] = q0 + i * 3 * H;
            memset(w->sc, 0, (size_t)t * (size_t)t * sizeof(float));
            ray_lin_gemm_acc_f32(w->rows, w->krows, w->sc, t, dh, t);
            for (int64_t i = 0; i < t; i++) {
                float* r = w->sc + i * t;
                float mx = r[0];
                for (int64_t j = 1; j < t; j++) mx = r[j] > mx ? r[j] : mx;
                float sum = 0.0f;
                for (int64_t j = 0; j < t; j++) { r[j] = emb_exp(r[j] - mx); sum += r[j]; }
                float inv = 1.0f / sum;
                for (int64_t j = 0; j < t; j++) r[j] *= inv;
            }
            /* the weighted values: t x dh, then into this head's columns */
            for (int64_t i = 0; i < t; i++) w->rows[i] = w->sc + i * t;
            /* the values of this head are rows already, t of them, in qkv */
            for (int64_t i = 0; i < t; i++) w->vrows[i] = v0 + i * 3 * H;
            memset(w->ch, 0, (size_t)t * (size_t)dh * sizeof(float));
            ray_lin_gemm_acc_f32(w->rows, w->vrows, w->ch, t, t, dh);
            for (int64_t i = 0; i < t; i++) memcpy(w->ctx + i * H + h * dh, w->ch + i * dh, (size_t)dh * sizeof(float));
        }
        /* attention's output, added to what came in, normalized */
        emb_dense(w->ctx, t, H, L->ao_r, L->ao_b, w->qkv, H, w->rows);
        for (int64_t i = 0; i < t * H; i++) x[i] += w->qkv[i];
        emb_layer_norm(x, t, H, L->ln1_w, L->ln1_b, m->eps);
        /* the wide layer, gelu, back to H, added, normalized */
        emb_dense(x, t, H, L->in_r, L->in_b, w->mid, I, w->rows);
        for (int64_t i = 0; i < t * I; i++) w->mid[i] = emb_gelu(w->mid[i]);
        emb_dense(w->mid, t, I, L->out_r, L->out_b, w->qkv, H, w->rows);
        for (int64_t i = 0; i < t * H; i++) x[i] += w->qkv[i];
        emb_layer_norm(x, t, H, L->ln2_w, L->ln2_b, m->eps);
    }
    for (int64_t j = 0; j < H; j++) out[j] = 0.0f;
    for (int64_t i = 0; i < t; i++) { const float* r = x + i * H; for (int64_t j = 0; j < H; j++) out[j] += r[j]; }
    float nn = 0.0f;
    for (int64_t j = 0; j < H; j++) { out[j] /= (float)t; nn += out[j] * out[j]; }
    float inv = nn > 0.0f ? 1.0f / __builtin_sqrtf(nn) : 0.0f;
    for (int64_t j = 0; j < H; j++) out[j] *= inv;
}

int64_t ray_embmodel_encode(const ray_embmodel_t* m, ray_embwork_t* w, const char* text, size_t len,
                            int64_t max_tokens, float* out) {
    if (!m->layer) return -2;
    int64_t n = emb_tok_encode(m->tok, &w->scratch, text, len);
    if (n < 0) return -1;
    int64_t cap = max_tokens > 0 && max_tokens < m->max_tokens ? max_tokens : m->max_tokens;
    if (cap < 2) cap = 2;
    if (n > cap) {
        /* The beginning is kept, and the end mark with it. */
        w->scratch.ids[cap - 1] = m->tok->eos;
        n = cap;
    }
    if (!emb_work_fit(m, w, n)) return -1;
    emb_forward(m, w, w->scratch.ids, n, out);
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

/* ---- the weights ---------------------------------------------------------- */

/* A block the model owns until it is freed; NULL when memory runs out. */
static void* emb_own(ray_embmodel_t* m, size_t bytes) {
    void* p = ray_sys_alloc(bytes ? bytes : 1);
    if (!p) return NULL;
    void** grown = (void**)ray_sys_alloc(((size_t)m->n_owned + 1) * sizeof(void*));
    if (!grown) { ray_sys_free(p); return NULL; }
    if (m->owned) { memcpy(grown, m->owned, (size_t)m->n_owned * sizeof(void*)); ray_sys_free(m->owned); }
    m->owned = grown;
    m->owned[m->n_owned++] = p;
    return p;
}

typedef struct {
    ray_t* head;          /* the file's table of tensors, parsed */
    const char* base;     /* where the tensors' bytes begin */
    size_t room;          /* and how many there are */
    const char* prefix;   /* some files name every tensor under "bert." */
} emb_file_t;

/* The F32 tensor `name` of the shape given (cols 0 for a vector), in the
 * mapping; NULL when it is missing or is not that. */
static const float* emb_tensor(const emb_file_t* f, const char* name, int64_t rows, int64_t cols) {
    char full[160];
    int k = snprintf(full, sizeof full, "%s%s", f->prefix, name);
    if (k < 0 || (size_t)k >= sizeof full) return NULL;
    const float* out = NULL;
    ray_t *key = ray_str(full, (size_t)k), *t = NULL, *dt = NULL, *sh = NULL, *off = NULL, *kk = NULL;
    if (!key || RAY_IS_ERR(key)) return NULL;
    t = ray_dict_get(f->head, key);
    ray_release(key);
    if (!t || RAY_IS_ERR(t) || t->type != RAY_DICT) goto done;
#define EMB_GET(dst, lit) do { kk = ray_str(lit, sizeof(lit) - 1); dst = kk && !RAY_IS_ERR(kk) ? ray_dict_get(t, kk) : NULL; if (kk) ray_release(kk); } while (0)
    EMB_GET(dt, "dtype"); EMB_GET(sh, "shape"); EMB_GET(off, "data_offsets");
#undef EMB_GET
    if (!emb_is_str(dt) || ray_str_len(dt) != 3 || memcmp(ray_str_ptr(dt), "F32", 3) != 0) goto done;
    if (!sh || sh->type != RAY_I64 || !off || off->type != RAY_I64 || off->len != 2) goto done;
    const int64_t* shape = (const int64_t*)ray_data(sh);
    if (cols == 0 ? !(sh->len == 1 && shape[0] == rows) : !(sh->len == 2 && shape[0] == rows && shape[1] == cols)) goto done;
    const int64_t* o = (const int64_t*)ray_data(off);
    size_t want = (size_t)rows * (size_t)(cols ? cols : 1) * sizeof(float);
    if (o[0] < 0 || o[1] < o[0] || (size_t)(o[1] - o[0]) != want || (size_t)o[1] > f->room) goto done;
    const char* at = f->base + o[0];
    if (((uintptr_t)at & (sizeof(float) - 1)) != 0) goto done;   /* a file that lays floats off their boundary is not read */
    out = (const float*)(const void*)at;
done:
    if (t) ray_release(t);
    if (dt) ray_release(dt);
    if (sh) ray_release(sh);
    if (off) ray_release(off);
    return out;
}

/* A dense weight as the file has it, [out x in], turned to [in x out] into
 * dst at a column offset, dst being `stride` wide. */
static void emb_turn(const float* w, int64_t out, int64_t in, float* dst, int64_t stride, int64_t at) {
    for (int64_t o = 0; o < out; o++) {
        const float* row = w + o * in;
        for (int64_t i = 0; i < in; i++) dst[i * stride + at + o] = row[i];
    }
}

static const float** emb_rows(ray_embmodel_t* m, const float* b, int64_t k, int64_t stride) {
    const float** r = (const float**)emb_own(m, (size_t)k * sizeof(float*));
    if (!r) return NULL;
    for (int64_t i = 0; i < k; i++) r[i] = b + i * stride;
    return r;
}

static float* emb_copy(ray_embmodel_t* m, const float* src, int64_t n) {
    float* p = src ? (float*)emb_own(m, (size_t)n * sizeof(float)) : NULL;
    if (p) memcpy(p, src, (size_t)n * sizeof(float));
    return p;
}

/* Maps model.safetensors and lays the encoder's weights out.  False with
 * *err set when the file is missing, is not in form, or lacks a tensor. */
static bool emb_read_weights(ray_embmodel_t* m, const char* dir, ray_t* limits, ray_t** err) {
    char path[1200];
    int pk = snprintf(path, sizeof path, "%s/model.safetensors", dir);
    if (pk < 0 || (size_t)pk >= sizeof path) { *err = ray_error("domain", ".emb.model.get: the path of model.safetensors is too long"); return false; }
    m->map = ray_vm_map_file(path, &m->map_len);
    if (!m->map) { *err = ray_error("io", ".emb.model.get: cannot map %s", path); return false; }
    uint64_t hlen = 0;
    if (m->map_len >= 8) memcpy(&hlen, m->map, 8);
    if (m->map_len < 8 || hlen == 0 || hlen > m->map_len - 8) { *err = ray_error("domain", ".emb.model.get: %s is not a safetensors file", path); return false; }
    ray_t* text = ray_str((const char*)m->map + 8, (size_t)hlen);
    if (!text || RAY_IS_ERR(text)) { *err = text ? text : ray_error("oom", NULL); return false; }
    ray_t* args[2] = { text, limits };
    ray_t* head = ray_json_parse_fn(args, 2);
    ray_release(text);
    if (!head || RAY_IS_ERR(head) || head->type != RAY_DICT) {
        if (head) ray_release(head);
        *err = ray_error("domain", ".emb.model.get: the table of tensors in %s cannot be read", path);
        return false;
    }
    emb_file_t f = { .head = head, .base = (const char*)m->map + 8 + hlen, .room = m->map_len - 8 - (size_t)hlen, .prefix = "" };
    const int64_t H = m->dim, I = m->inner, P = m->max_tokens;
    const char* missing = NULL;
    bool ok = false;
#define EMB_NEED(dst, name, r, c) do { dst = emb_tensor(&f, name, r, c); if (!dst) { missing = name; goto done; } } while (0)
    /* The first table tells how many tokens the weights know, and whether the names carry a prefix. */
    {
        static const char* const prefixes[] = { "", "bert.", "roberta.", "model." };
        int64_t words = 0;
        for (size_t pi = 0; pi < sizeof prefixes / sizeof prefixes[0] && words == 0; pi++) {
            char name[160];
            int k = snprintf(name, sizeof name, "%sembeddings.word_embeddings.weight", prefixes[pi]);
            ray_t* key = ray_str(name, (size_t)k);
            ray_t* t = key && !RAY_IS_ERR(key) ? ray_dict_get(head, key) : NULL;
            if (key) ray_release(key);
            if (t && !RAY_IS_ERR(t) && t->type == RAY_DICT) {
                ray_t* kk = ray_str("shape", 5);
                ray_t* sh = ray_dict_get(t, kk);
                ray_release(kk);
                if (sh && sh->type == RAY_I64 && sh->len == 2 && ((const int64_t*)ray_data(sh))[1] == H) {
                    words = ((const int64_t*)ray_data(sh))[0];
                    f.prefix = prefixes[pi];
                }
                if (sh) ray_release(sh);
            }
            if (t) ray_release(t);
        }
        if (words <= 0) { missing = "embeddings.word_embeddings.weight"; goto done; }
        m->words = words;
    }
    const float *w, *b;
    EMB_NEED(m->word, "embeddings.word_embeddings.weight", m->words, H);
    EMB_NEED(m->pos,  "embeddings.position_embeddings.weight", P, H);
    EMB_NEED(w, "embeddings.token_type_embeddings.weight", 2, H);
    m->type = w;
    EMB_NEED(w, "embeddings.LayerNorm.weight", H, 0);
    EMB_NEED(b, "embeddings.LayerNorm.bias", H, 0);
    m->eln_w = emb_copy(m, w, H); m->eln_b = emb_copy(m, b, H);
    m->layer = (emb_layer_t*)ray_sys_alloc((size_t)m->layers * sizeof(emb_layer_t));
    if (!m->eln_w || !m->eln_b || !m->layer) { *err = ray_error("oom", NULL); goto done; }
    for (int64_t l = 0; l < m->layers; l++) {
        emb_layer_t* L = &m->layer[l];
        char name[160];
#define EMB_L(dst, suffix, r, c) do { snprintf(name, sizeof name, "encoder.layer.%lld.%s", (long long)l, suffix); dst = emb_tensor(&f, name, r, c); if (!dst) { missing = suffix; goto done; } } while (0)
        const float *qw, *qb, *kw, *kb, *vw, *vb, *aw, *ab, *iw, *ib, *ow, *ob, *n1w, *n1b, *n2w, *n2b;
        EMB_L(qw, "attention.self.query.weight", H, H); EMB_L(qb, "attention.self.query.bias", H, 0);
        EMB_L(kw, "attention.self.key.weight", H, H);   EMB_L(kb, "attention.self.key.bias", H, 0);
        EMB_L(vw, "attention.self.value.weight", H, H); EMB_L(vb, "attention.self.value.bias", H, 0);
        EMB_L(aw, "attention.output.dense.weight", H, H); EMB_L(ab, "attention.output.dense.bias", H, 0);
        EMB_L(n1w, "attention.output.LayerNorm.weight", H, 0); EMB_L(n1b, "attention.output.LayerNorm.bias", H, 0);
        EMB_L(iw, "intermediate.dense.weight", I, H); EMB_L(ib, "intermediate.dense.bias", I, 0);
        EMB_L(ow, "output.dense.weight", H, I); EMB_L(ob, "output.dense.bias", H, 0);
        EMB_L(n2w, "output.LayerNorm.weight", H, 0); EMB_L(n2b, "output.LayerNorm.bias", H, 0);
#undef EMB_L
        L->qkv = (float*)emb_own(m, (size_t)H * 3 * (size_t)H * sizeof(float));
        L->qkv_b = (float*)emb_own(m, 3 * (size_t)H * sizeof(float));
        L->ao  = (float*)emb_own(m, (size_t)H * (size_t)H * sizeof(float));
        L->in  = (float*)emb_own(m, (size_t)H * (size_t)I * sizeof(float));
        L->out = (float*)emb_own(m, (size_t)I * (size_t)H * sizeof(float));
        if (!L->qkv || !L->qkv_b || !L->ao || !L->in || !L->out) { *err = ray_error("oom", NULL); goto done; }
        emb_turn(qw, H, H, L->qkv, 3 * H, 0); emb_turn(kw, H, H, L->qkv, 3 * H, H); emb_turn(vw, H, H, L->qkv, 3 * H, 2 * H);
        memcpy(L->qkv_b, qb, (size_t)H * sizeof(float)); memcpy(L->qkv_b + H, kb, (size_t)H * sizeof(float)); memcpy(L->qkv_b + 2 * H, vb, (size_t)H * sizeof(float));
        emb_turn(aw, H, H, L->ao, H, 0);
        emb_turn(iw, I, H, L->in, I, 0);
        emb_turn(ow, H, I, L->out, H, 0);
        L->ao_b = emb_copy(m, ab, H); L->in_b = emb_copy(m, ib, I); L->out_b = emb_copy(m, ob, H);
        L->ln1_w = emb_copy(m, n1w, H); L->ln1_b = emb_copy(m, n1b, H); L->ln2_w = emb_copy(m, n2w, H); L->ln2_b = emb_copy(m, n2b, H);
        L->qkv_r = emb_rows(m, L->qkv, H, 3 * H); L->ao_r = emb_rows(m, L->ao, H, H);
        L->in_r = emb_rows(m, L->in, H, I); L->out_r = emb_rows(m, L->out, I, H);
        if (!L->ao_b || !L->in_b || !L->out_b || !L->ln1_w || !L->ln1_b || !L->ln2_w || !L->ln2_b || !L->qkv_r || !L->ao_r || !L->in_r || !L->out_r) {
            *err = ray_error("oom", NULL); goto done;
        }
    }
    ok = true;
done:
#undef EMB_NEED
    ray_release(head);
    if (!ok && !*err)
        *err = ray_error("domain", ".emb.model.get: %s has no F32 tensor %s%s of the shape config.json describes", path, f.prefix, missing ? missing : "?");
    return ok;
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
    {
        ray_t* e = emb_field(cfg, "layer_norm_eps", limits);
        m->eps = e && !RAY_IS_ERR(e) && e->type == -RAY_F64 ? (float)e->f64 : 1e-12f;
        if (e) ray_release(e);
    }
    /* Positions that begin after a padding index, as the roberta family counts them, are not read yet. */
    if (strcmp(m->kind, "bert") != 0) {
        *err = ray_error("domain", ".emb.model.get: config.json names a model of the kind '%s'; the one kind read is bert", m->kind[0] ? m->kind : "?");
        goto fail;
    }
    m->tok = emb_read_tokenizer(dir, limits, err);
    if (!m->tok) goto fail;
    if (!emb_read_weights(m, dir, limits, err)) goto fail;
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
