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
#include "core/pool.h"
#include <math.h>

/* One layer of the encoder.  Every pointer is into the mapped file: a dense
 * weight lies there as [out x in], and is computed with where it lies. */
typedef struct {
    const float *qw, *qb, *kw, *kb, *vw, *vb;   /* H x H and H, each */
    const float *aw, *ab;                       /* H x H */
    const float *iw, *ib;                       /* I x H */
    const float *ow, *ob;                       /* H x I */
    const float *ln1_w, *ln1_b, *ln2_w, *ln2_b; /* H each */
} emb_layer_t;

struct ray_embmodel {
    _Atomic(int64_t) rc;
    emb_tok_t* tok;
    int64_t dim, layers, heads, inner, max_tokens;
    char kind[32];
    /* the weights */
    void* map; size_t map_len;                 /* model.safetensors, mapped */
    const float *word, *pos, *type;            /* the three tables, in the mapping */
    int64_t words, types;                      /* rows of `word` and of `type` */
    const float *eln_w, *eln_b;                /* the embeddings' layer norm */
    float eps;
    emb_layer_t* layer;                        /* the one block the weights own: pointers */
};

/* What one head of attention needs while it is computed; a worker has one. */
typedef struct {
    float* sc;            /* the weights of attention, t x t */
} emb_lane_t;

struct ray_embwork {
    emb_scratch_t scratch;
    /* the forward pass, sized for `cap` tokens and `lanes` workers */
    int64_t cap, lanes;
    float *x, *qkv, *ctx, *mid;   /* qkv: query, key and value, a block of t x H each */
    float* tr;                    /* a dense layer's result on its side, inner x t */
    emb_lane_t* lane;
};

void ray_embmodel_retain(ray_embmodel_t* m) {
    if (m) atomic_fetch_add_explicit(&m->rc, 1, memory_order_relaxed);
}

void ray_embmodel_release(ray_embmodel_t* m) {
    if (!m) return;
    if (atomic_fetch_sub_explicit(&m->rc, 1, memory_order_acq_rel) != 1) return;
    emb_tok_free(m->tok);
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
    for (int64_t i = 0; w->lane && i < w->lanes; i++) {
        emb_lane_t* l = &w->lane[i];
        emb_free(l->sc);
    }
    emb_free(w->lane);
    emb_free(w->x); emb_free(w->qkv); emb_free(w->ctx); emb_free(w->mid); emb_free(w->tr);
    w->x = w->qkv = w->ctx = w->mid = w->tr = NULL;
    w->lane = NULL;
    w->cap = w->lanes = 0;
}

void ray_embwork_free(ray_embwork_t* w) {
    if (!w) return;
    emb_scratch_free(&w->scratch);
    emb_work_drop(w);
    ray_sys_free(w);
}

/* Rows of zeros a buffer of the pass has room for after the text's. */
#define EMB_PAD 8

/* Room for a text of t tokens, computed by `lanes` workers at once. */
static bool emb_work_fit(const ray_embmodel_t* m, ray_embwork_t* w, int64_t t, int64_t lanes) {
    if (t <= w->cap && lanes <= w->lanes) return true;
    if (t < w->cap) t = w->cap;
    if (lanes < w->lanes) lanes = w->lanes;
    emb_work_drop(w);
    size_t T = (size_t)t + EMB_PAD, H = (size_t)m->dim, I = (size_t)m->inner;
    w->x   = (float*)ray_sys_alloc(T * H * sizeof(float));
    w->qkv = (float*)ray_sys_alloc(T * 3 * H * sizeof(float));
    w->ctx = (float*)ray_sys_alloc(T * H * sizeof(float));
    w->mid = (float*)ray_sys_alloc(T * I * sizeof(float));
    w->tr  = (float*)ray_sys_alloc(T * (I > H ? I : H) * sizeof(float));
    w->lane = (emb_lane_t*)ray_sys_alloc((size_t)lanes * sizeof(emb_lane_t));   /* zero-filled */
    bool ok = w->x && w->qkv && w->ctx && w->mid && w->tr && w->lane;
    if (ok) w->lanes = lanes;   /* so that a drop frees what the loop below made */
    for (int64_t i = 0; ok && i < lanes; i++) {
        emb_lane_t* l = &w->lane[i];
        l->sc = (float*)ray_sys_alloc(T * T * sizeof(float));
        ok = l->sc != NULL;
    }
    if (!ok) { emb_work_drop(w); return false; }
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

/* A dense layer over the rows of a: c[t x cols] = bias + a * W', then gelu
 * where the layer has it, W being as the file has it, [cols x k].
 *
 * It is computed the other way up, as W * a' into tr[cols x t]: the kernel
 * (ops/linalg.c) packs the strip of the right-hand matrix it works on, and
 * this way round that is a strip of a, a few rows of the text, while the
 * rows of W pass over it four at a time straight from the mapped file.  The
 * packing is then a 1/cols part of the work whatever the length of the text;
 * the other way round it is a 1/t part, which a short text pays dearly.  One
 * task computes a run of W's rows, which are columns of c. */
typedef struct {
    const float *a, *w, *bias;
    float *c, *tr;
    int64_t t, ts, k, cols, per;   /* ts: t rounded up to a whole vector, the width of tr */
    bool gelu;
} emb_dense_t;

static void emb_dense_task(void* vctx, uint32_t worker, int64_t start, int64_t end) {
    const emb_dense_t* d = (const emb_dense_t*)vctx;
    (void)worker; (void)end;
    int64_t lo = start * d->per, hi = lo + d->per < d->cols ? lo + d->per : d->cols;
    const int64_t t = d->t, ts = d->ts;
    float* tr = d->tr + lo * ts;
    memset(tr, 0, (size_t)(hi - lo) * (size_t)ts * sizeof(float));
    /* ts columns, the text's rows and the rows of zeros after them: whole vectors, no column left over */
    ray_lin_mul_acc_f32(d->w + lo * d->k, d->k, hi - lo, d->k, d->a, d->k, true, tr, ts, 0, ts);
    /* back the right way up, a few columns at a time so that both sides are read and written in runs */
    for (int64_t j0 = lo; j0 < hi; j0 += 16) {
        int64_t j1 = j0 + 16 < hi ? j0 + 16 : hi;
        for (int64_t i = 0; i < t; i++) {
            float* r = d->c + i * d->cols;
            for (int64_t j = j0; j < j1; j++) r[j] = d->bias[j] + d->tr[j * ts + i];
        }
    }
    if (d->gelu)
        for (int64_t i = 0; i < t; i++) {
            float* r = d->c + i * d->cols;
            for (int64_t j = lo; j < hi; j++) r[j] = emb_gelu(r[j]);
        }
}

/* Rows of W a worker takes at once: a multiple of four, as the kernel blocks them. */
#define EMB_RUN_ROWS 16

static void emb_dense(ray_pool_t* pool, float* a, int64_t t, int64_t k, const float* w,
                      const float* bias, float* c, int64_t cols, float* tr, bool gelu) {
    /* The rows after the text's, up to a whole vector of them, are made zeros:
     * every buffer of the pass has room for them (EMB_PAD). */
    int64_t ts = (t + 7) & ~(int64_t)7;
    if (ts > t) memset(a + t * k, 0, (size_t)(ts - t) * (size_t)k * sizeof(float));
    emb_dense_t d = { .a = a, .w = w, .bias = bias, .c = c, .tr = tr, .t = t, .ts = ts, .k = k, .cols = cols, .per = cols, .gelu = gelu };
    if (pool) {
        int64_t nw = (int64_t)ray_pool_total_workers(pool);
        int64_t per = (cols + nw * 2 - 1) / (nw * 2);
        per = (per + 3) & ~(int64_t)3;
        if (per < EMB_RUN_ROWS) per = EMB_RUN_ROWS;
        int64_t nt = (cols + per - 1) / per;
        if (nt > 1) { d.per = per; ray_pool_dispatch_n(pool, emb_dense_task, &d, (uint32_t)nt); return; }
    }
    emb_dense_task(&d, 0, 0, 1);
}

/* One row of scores into weights that sum to one: exp of each, scaled, less
 * the greatest.  A function of its own, out of line: inside the loop over
 * rows the vectoriser did not make of it what it makes of it here. */
static __attribute__((noinline)) void emb_softmax(float* restrict r, int64_t t, float scale) {
    /* The greatest, kept in eight places at once: one running maximum is a
     * chain in which every comparison waits for the one before it. */
    float mx = r[0];
    int64_t j0 = 0;
    if (t >= 8) {
        float m8[8];
        for (int q = 0; q < 8; q++) m8[q] = r[q];
        for (j0 = 8; j0 + 8 <= t; j0 += 8)
            for (int q = 0; q < 8; q++) m8[q] = r[j0 + q] > m8[q] ? r[j0 + q] : m8[q];
        for (int q = 0; q < 8; q++) mx = m8[q] > mx ? m8[q] : mx;
    }
    for (int64_t j = j0; j < t; j++) mx = r[j] > mx ? r[j] : mx;
    float sum = 0.0f;
    for (int64_t j = 0; j < t; j++) { r[j] = emb_exp((r[j] - mx) * scale); sum += r[j]; }
    float inv = 1.0f / sum;
    for (int64_t j = 0; j < t; j++) r[j] *= inv;
}

/* Attention: every head from the query, key and value of every token (the
 * three blocks of qkv) into its own columns of ctx.  One task computes one head. */
typedef struct {
    const ray_embwork_t* w;
    int64_t t, H, dh;
} emb_heads_t;

static void emb_head_task(void* vctx, uint32_t worker, int64_t h, int64_t end) {
    const emb_heads_t* a = (const emb_heads_t*)vctx;
    (void)end;
    const ray_embwork_t* w = a->w;
    emb_lane_t* l = &w->lane[worker];
    const int64_t t = a->t, H = a->H, dh = a->dh;
    const float scale = 1.0f / __builtin_sqrtf((float)dh);
    const float* q0 = w->qkv + h * dh;
    const float* k0 = w->qkv + t * H + h * dh;
    const float* v0 = w->qkv + 2 * t * H + h * dh;
    /* every query of this head against every key of it, where they lie */
    memset(l->sc, 0, (size_t)t * (size_t)t * sizeof(float));
    ray_lin_mul_acc_f32(q0, H, t, dh, k0, H, true, l->sc, t, 0, t);
    for (int64_t i = 0; i < t; i++) emb_softmax(l->sc + i * t, t, scale);
    /* the weighted values, straight into this head's columns of ctx */
    float* c0 = w->ctx + h * dh;
    for (int64_t i = 0; i < t; i++) memset(c0 + i * H, 0, (size_t)dh * sizeof(float));
    ray_lin_mul_acc_f32(l->sc, t, t, t, v0, H, false, c0, H, 0, dh);
}

/* The vector of the tokens `ids` (t of them, t <= w->cap): the encoder's
 * last layer averaged over the tokens, scaled to length one.  With a pool
 * the rows of each product and the heads of attention go to its workers;
 * w then has a lane for every one of them. */
static void emb_forward(const ray_embmodel_t* m, ray_embwork_t* w, ray_pool_t* pool,
                        const int32_t* ids, int64_t t, float* out) {
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
    emb_heads_t heads = { .w = w, .t = t, .H = H, .dh = dh };
    for (int64_t l = 0; l < m->layers; l++) {
        const emb_layer_t* L = &m->layer[l];
        float *q = w->qkv, *k = q + t * H, *v = k + t * H;
        emb_dense(pool, x, t, H, L->qw, L->qb, q, H, w->tr, false);
        emb_dense(pool, x, t, H, L->kw, L->kb, k, H, w->tr, false);
        emb_dense(pool, x, t, H, L->vw, L->vb, v, H, w->tr, false);
        if (pool && nh > 1) ray_pool_dispatch_n(pool, emb_head_task, &heads, (uint32_t)nh);
        else for (int64_t h = 0; h < nh; h++) emb_head_task(&heads, 0, h, h + 1);
        /* attention's output, added to what came in, normalized; q is free again */
        emb_dense(pool, w->ctx, t, H, L->aw, L->ab, q, H, w->tr, false);
        for (int64_t i = 0; i < t * H; i++) x[i] += q[i];
        emb_layer_norm(x, t, H, L->ln1_w, L->ln1_b, m->eps);
        /* the wide layer with gelu, back to H, added, normalized */
        emb_dense(pool, x, t, H, L->iw, L->ib, w->mid, I, w->tr, true);
        emb_dense(pool, w->mid, t, I, L->ow, L->ob, q, H, w->tr, false);
        for (int64_t i = 0; i < t * H; i++) x[i] += q[i];
        emb_layer_norm(x, t, H, L->ln2_w, L->ln2_b, m->eps);
    }
    for (int64_t j = 0; j < H; j++) out[j] = 0.0f;
    for (int64_t i = 0; i < t; i++) { const float* r = x + i * H; for (int64_t j = 0; j < H; j++) out[j] += r[j]; }
    float nn = 0.0f;
    for (int64_t j = 0; j < H; j++) { out[j] /= (float)t; nn += out[j] * out[j]; }
    float inv = nn > 0.0f ? 1.0f / __builtin_sqrtf(nn) : 0.0f;
    for (int64_t j = 0; j < H; j++) out[j] *= inv;
}

/* Below this many tokens a text is computed by the one thread that has it:
 * its products are too small for a dispatch to pay. */
#define EMB_PAR_TOKENS 8

int64_t ray_embmodel_encode(const ray_embmodel_t* m, ray_embwork_t* w, ray_pool_t* pool,
                            const char* text, size_t len, int64_t max_tokens, float* out) {
    int64_t n = emb_tok_encode(m->tok, &w->scratch, text, len);
    if (n < 0) return -1;
    int64_t cap = max_tokens > 0 && max_tokens < m->max_tokens ? max_tokens : m->max_tokens;
    if (cap < 2) cap = 2;
    if (n > cap) {
        /* The beginning is kept, and the end mark with it. */
        w->scratch.ids[cap - 1] = m->tok->eos;
        n = cap;
    }
    if (pool && n < EMB_PAR_TOKENS) pool = NULL;
    if (!emb_work_fit(m, w, n, pool ? (int64_t)ray_pool_total_workers(pool) : 1)) return -1;
    emb_forward(m, w, pool, w->scratch.ids, n, out);
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
    if (RAY_IS_ERR(s)) {   /* the read's error, owned here: freed (ray_release leaves it, #1854) */
        bool oom = strcmp(ray_err_code(s), "oom") == 0;
        ray_error_free(s);
        return oom ? ray_error("oom", NULL) : ray_error("io", ".emb.model.get: cannot read %s", path);
    }
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
    /* keys and vals are owned here and hold 3 slots, so the appends do not
     * grow; a value that fails to allocate is the failure, never a limit
     * read as an error object (#1854) */
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        int64_t s = sym_intern_safe(rows[i].name, strlen(rows[i].name));
        ray_t* v = make_i64(rows[i].v);
        if (s < 0 || !v || RAY_IS_ERR(v)) {
            ray_error_free(v);
            ray_release(keys); ray_release(vals);
            return ray_error("oom", NULL);
        }
        ray_t* nk = ray_vec_append(keys, &s);
        ray_t* nl = (nk && !RAY_IS_ERR(nk)) ? ray_list_append(vals, v) : NULL;
        ray_release(v);
        if (!nk || RAY_IS_ERR(nk) || !nl || RAY_IS_ERR(nl)) {
            ray_error_free(nk); ray_error_free(nl);
            ray_release(nk && !RAY_IS_ERR(nk) ? nk : keys);
            ray_release(vals);
            return ray_error("oom", NULL);
        }
        keys = nk; vals = nl;
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

/* A whole number held in a field, or `miss` when the field is not one.
 * *oom is set when reading it failed to allocate: never `miss` in its
 * place (#1865). */
static int64_t emb_int(ray_t* doc, const char* path, ray_t* limits, int64_t miss, bool* oom) {
    ray_t* v = emb_field(doc, path, limits);
    int64_t out = miss;
    if (v && RAY_IS_ERR(v) && strcmp(ray_err_code(v), "oom") == 0) *oom = true;
    if (v && !RAY_IS_ERR(v)) {
        if (v->type == -RAY_I64) out = v->i64;
        else if (v->type == -RAY_F64 && v->f64 == (double)(int64_t)v->f64) out = (int64_t)v->f64;
    }
    if (v && RAY_IS_ERR(v)) ray_error_free(v); else if (v) ray_release(v);   /* owned here; ray_release leaves an error (#1854) */
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

typedef struct {
    ray_t* head;          /* the file's table of tensors, parsed */
    const char* base;     /* where the tensors' bytes begin */
    size_t room;          /* and how many there are */
    const char* prefix;   /* some files name every tensor under "bert." */
    bool oom;             /* a lookup failed to allocate: the load is oom, not a tensor missing (#1865) */
} emb_file_t;

/* The F32 tensor `name` of the shape given (cols 0 for a vector), in the
 * mapping; NULL when it is missing or is not that. */
static const float* emb_tensor(emb_file_t* f, const char* name, int64_t rows, int64_t cols) {
    char full[160];
    int k = snprintf(full, sizeof full, "%s%s", f->prefix, name);
    if (k < 0 || (size_t)k >= sizeof full) return NULL;
    const float* out = NULL;
    ray_t *key = ray_str(full, (size_t)k), *t = NULL, *dt = NULL, *sh = NULL, *off = NULL, *kk = NULL;
    if (!key || RAY_IS_ERR(key)) { ray_error_free(key); f->oom = true; return NULL; }   /* owned here */
    t = ray_dict_get(f->head, key);
    ray_release(key);
    if (t && RAY_IS_ERR(t)) { ray_error_free(t); t = NULL; f->oom = true; goto done; }
    if (!t || t->type != RAY_DICT) goto done;
    /* each key and lookup is owned here: a failed one marks the load oom
     * and its error is freed (ray_release leaves it) */
#define EMB_GET(dst, lit) do { \
        kk = ray_str(lit, sizeof(lit) - 1); \
        if (!kk || RAY_IS_ERR(kk)) { ray_error_free(kk); kk = NULL; f->oom = true; goto done; } \
        dst = ray_dict_get(t, kk); ray_release(kk); kk = NULL; \
        if (dst && RAY_IS_ERR(dst)) { ray_error_free(dst); dst = NULL; f->oom = true; goto done; } \
    } while (0)
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

/* Maps model.safetensors and finds the encoder's weights in it: nothing is
 * copied, every tensor is used where the mapping has it.  False with
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
        /* head is owned here: an oom is the failure, never "cannot be
         * read"; an error is freed (ray_release leaves it, #1865) */
        bool oom = head && RAY_IS_ERR(head) && strcmp(ray_err_code(head), "oom") == 0;
        if (head && RAY_IS_ERR(head)) ray_error_free(head); else if (head) ray_release(head);
        *err = oom ? ray_error("oom", NULL) : ray_error("domain", ".emb.model.get: the table of tensors in %s cannot be read", path);
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
            if (!key || RAY_IS_ERR(key)) { ray_error_free(key); f.oom = true; goto done; }   /* owned here (#1865) */
            ray_t* t = ray_dict_get(head, key);
            ray_release(key);
            if (t && RAY_IS_ERR(t)) { ray_error_free(t); f.oom = true; goto done; }
            if (t && t->type == RAY_DICT) {
                ray_t* kk = ray_str("shape", 5);
                if (!kk || RAY_IS_ERR(kk)) { ray_error_free(kk); ray_release(t); f.oom = true; goto done; }
                ray_t* sh = ray_dict_get(t, kk);
                ray_release(kk);
                if (sh && RAY_IS_ERR(sh)) { ray_error_free(sh); ray_release(t); f.oom = true; goto done; }
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
    EMB_NEED(m->word, "embeddings.word_embeddings.weight", m->words, H);
    EMB_NEED(m->pos,  "embeddings.position_embeddings.weight", P, H);
    EMB_NEED(m->type, "embeddings.token_type_embeddings.weight", m->types, H);
    EMB_NEED(m->eln_w, "embeddings.LayerNorm.weight", H, 0);
    EMB_NEED(m->eln_b, "embeddings.LayerNorm.bias", H, 0);
    m->layer = (emb_layer_t*)ray_sys_alloc((size_t)m->layers * sizeof(emb_layer_t));
    if (!m->layer) { *err = ray_error("oom", NULL); goto done; }
    for (int64_t l = 0; l < m->layers; l++) {
        emb_layer_t* L = &m->layer[l];
        char name[160];
#define EMB_L(dst, suffix, r, c) do { snprintf(name, sizeof name, "encoder.layer.%lld.%s", (long long)l, suffix); dst = emb_tensor(&f, name, r, c); if (!dst) { missing = suffix; goto done; } } while (0)
        EMB_L(L->qw, "attention.self.query.weight", H, H); EMB_L(L->qb, "attention.self.query.bias", H, 0);
        EMB_L(L->kw, "attention.self.key.weight", H, H);   EMB_L(L->kb, "attention.self.key.bias", H, 0);
        EMB_L(L->vw, "attention.self.value.weight", H, H); EMB_L(L->vb, "attention.self.value.bias", H, 0);
        EMB_L(L->aw, "attention.output.dense.weight", H, H); EMB_L(L->ab, "attention.output.dense.bias", H, 0);
        EMB_L(L->ln1_w, "attention.output.LayerNorm.weight", H, 0); EMB_L(L->ln1_b, "attention.output.LayerNorm.bias", H, 0);
        EMB_L(L->iw, "intermediate.dense.weight", I, H); EMB_L(L->ib, "intermediate.dense.bias", I, 0);
        EMB_L(L->ow, "output.dense.weight", H, I); EMB_L(L->ob, "output.dense.bias", H, 0);
        EMB_L(L->ln2_w, "output.LayerNorm.weight", H, 0); EMB_L(L->ln2_b, "output.LayerNorm.bias", H, 0);
#undef EMB_L
    }
    ok = true;
done:
#undef EMB_NEED
    ray_release(head);
    if (!ok && !*err && f.oom) *err = ray_error("oom", NULL);   /* never a tensor missing in its place */
    if (!ok && !*err)
        *err = ray_error("domain", ".emb.model.get: %s has no F32 tensor %s%s of the shape config.json describes", path, f.prefix, missing ? missing : "?");
    return ok;
}

/* The member `name` of a parsed JSON object: owned, or NULL when v is not an
 * object or has no such member, or an owned `oom` error when reading it
 * failed to allocate (#1865): never "no such member" in its place. */
static ray_t* emb_member(ray_t* v, const char* name) {
    if (!v || RAY_IS_ERR(v) || v->type != RAY_DICT) return NULL;
    ray_t* k = ray_str(name, strlen(name));
    if (!k || RAY_IS_ERR(k)) { ray_error_free(k); return ray_error("oom", NULL); }
    ray_t* out = ray_dict_get(v, k);
    ray_release(k);
    if (out && RAY_IS_ERR(out)) {   /* owned here: an oom is the answer, any other error no member */
        bool oom = strcmp(ray_err_code(out), "oom") == 0;
        ray_error_free(out);
        return oom ? ray_error("oom", NULL) : NULL;
    }
    if (out && RAY_IS_NULL(out)) { ray_release(out); out = NULL; }
    return out;
}

/* *x, from emb_member, is the failure: handed to *err, the slot cleared. */
#define EMB_MEMBER_CHECK(x) do { if ((x) && RAY_IS_ERR(x)) { *err = (x); (x) = NULL; goto done; } } while (0)

/* The tokenizer described by tokenizer.json; NULL with *err set otherwise. */
static emb_tok_t* emb_read_tokenizer(const char* dir, ray_t* limits, ray_t** err) {
    emb_tok_t* tok = NULL;
    ray_t *doc = NULL, *model = NULL, *norm = NULL, *kind = NULL, *vocab = NULL, *map = NULL, *unk_v = NULL;
    uint8_t* charsmap = NULL; size_t charsmap_len = 0;
    const char** toks = NULL; uint32_t* lens = NULL; double* scores = NULL;
#define EMB_FAIL(...) do { *err = ray_error(__VA_ARGS__); goto done; } while (0)
    doc = emb_file(dir, "tokenizer.json");
    if (RAY_IS_ERR(doc)) { *err = doc; doc = NULL; goto done; }
    /* The file is read through twice, once for each of the two sections wanted, and no more. */
    model = emb_field(doc, "model", limits);
    if (model && RAY_IS_ERR(model)) { *err = model; model = NULL; goto done; }
    kind = emb_member(model, "type");
    EMB_MEMBER_CHECK(kind);
    if (!emb_is_str(kind) || ray_str_len(kind) != 7 || memcmp(ray_str_ptr(kind), "Unigram", 7) != 0)
        EMB_FAIL("domain", ".emb.model.get: tokenizer.json holds a tokenizer of another kind than Unigram, which is the one kind read");
    /* The table of replacements is the normalizer itself, or one of a sequence of them. */
    norm = emb_field(doc, "normalizer", limits);
    if (norm && RAY_IS_ERR(norm)) {
        /* an oom is the failure, never "no normalizer"; any other error
         * is read as no normalizer, as before.  Owned here (#1854). */
        if (strcmp(ray_err_code(norm), "oom") == 0) { *err = norm; norm = NULL; goto done; }
        ray_error_free(norm); norm = NULL;
    }
    ray_release(doc); doc = NULL;
    map = emb_member(norm, "precompiled_charsmap");
    EMB_MEMBER_CHECK(map);
    if (!map) {
        ray_t* seq = emb_member(norm, "normalizers");
        EMB_MEMBER_CHECK(seq);
        for (int64_t i = 0; seq && seq->type == RAY_LIST && i < seq->len && !map; i++)
            map = emb_member(ray_list_get(seq, i), "precompiled_charsmap");
        if (seq) ray_release(seq);
        EMB_MEMBER_CHECK(map);
    }
    if (map && !emb_is_str(map)) { ray_release(map); map = NULL; }
    if (map) {
        charsmap = emb_base64(ray_str_ptr(map), ray_str_len(map), &charsmap_len);
        if (!charsmap) EMB_FAIL("domain", ".emb.model.get: the tokenizer's table of replacements is not base64");
    }
    vocab = emb_member(model, "vocab");
    EMB_MEMBER_CHECK(vocab);
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
    unk_v = emb_member(model, "unk_id");
    EMB_MEMBER_CHECK(unk_v);
    int64_t unk = !unk_v ? -1 : unk_v->type == -RAY_I64 ? unk_v->i64 : unk_v->type == -RAY_F64 ? (int64_t)unk_v->f64 : -1;
    if (unk < 0 || unk >= n || bos < 0 || eos < 0)
        EMB_FAIL("domain", ".emb.model.get: the vocabulary lacks the token for the unknown, for the beginning (<s>) or for the end (</s>)");
    /* The call takes the decoded table for its own, made or not. */
    tok = emb_tok_new(charsmap, charsmap_len, toks, lens, scores, n, (int32_t)unk, bos, eos);
    charsmap = NULL;
    if (!tok) EMB_FAIL("domain", ".emb.model.get: the tokenizer's tables could not be built: not in form, or out of memory");
done:
#undef EMB_FAIL
    if (toks) ray_sys_free((void*)toks);
    if (lens) ray_sys_free(lens);
    if (scores) ray_sys_free(scores);
    if (charsmap) ray_sys_free(charsmap);
    if (map) ray_release(map);
    if (unk_v) ray_release(unk_v);
    if (norm) ray_release(norm);
    if (model) ray_release(model);
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
        if (kind && RAY_IS_ERR(kind)) {   /* owned here: an oom is the failure (#1865) */
            bool oom = strcmp(ray_err_code(kind), "oom") == 0;
            ray_error_free(kind);
            if (oom) { *err = ray_error("oom", NULL); goto fail; }
            kind = NULL;
        }
        if (emb_is_str(kind)) {
            size_t k = ray_str_len(kind) < sizeof m->kind - 1 ? ray_str_len(kind) : sizeof m->kind - 1;
            memcpy(m->kind, ray_str_ptr(kind), k);
        }
        if (kind) ray_release(kind);
    }
    bool field_oom = false;
    m->dim = emb_int(cfg, "hidden_size", limits, 0, &field_oom);
    m->layers = emb_int(cfg, "num_hidden_layers", limits, 0, &field_oom);
    m->heads = emb_int(cfg, "num_attention_heads", limits, 0, &field_oom);
    m->inner = emb_int(cfg, "intermediate_size", limits, 0, &field_oom);
    m->max_tokens = emb_int(cfg, "max_position_embeddings", limits, 0, &field_oom);
    m->types = emb_int(cfg, "type_vocab_size", limits, 2, &field_oom);
    if (field_oom) { *err = ray_error("oom", NULL); goto fail; }
    if (m->types < 1) m->types = 2;
    if (m->dim <= 0 || m->layers <= 0 || m->heads <= 0 || m->inner <= 0 || m->max_tokens < 3 || m->dim % m->heads != 0) {
        *err = ray_error("domain", ".emb.model.get: config.json does not describe an encoder: hidden_size, num_hidden_layers, num_attention_heads, intermediate_size and max_position_embeddings are needed");
        goto fail;
    }
    {
        ray_t* e = emb_field(cfg, "layer_norm_eps", limits);
        if (e && RAY_IS_ERR(e)) {   /* owned here: an oom is the failure (#1865) */
            bool oom = strcmp(ray_err_code(e), "oom") == 0;
            ray_error_free(e);
            if (oom) { *err = ray_error("oom", NULL); goto fail; }
            e = NULL;
        }
        m->eps = e && e->type == -RAY_F64 ? (float)e->f64 : 1e-12f;
        if (e) ray_release(e);
    }
    /* Other families count positions and name tensors in ways of their own. */
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
