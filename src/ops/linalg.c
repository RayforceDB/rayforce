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
#include "lang/internal.h"
#include "core/pool.h"
#include "mem/sys.h"

/* ==========================================================================
 *  Dense linear algebra builtins: mmu, inv, lsq, flip
 *
 *  A matrix is a RAY_LIST of equal-length numeric vectors, one per row; a
 *  bare numeric vector is a vector operand.  F64 and F32 rows are used in
 *  place; I32/I64 rows are widened to double.
 *
 *  Result type: F32 when every operand is F32, else F64.  All arithmetic
 *  accumulates in double, so F32 only halves the memory traffic and the
 *  result size; it does not cost accuracy in the sums.
 * ========================================================================== */

typedef struct {
    int64_t r, c;          /* rows, cols (a vector operand is r=1, c=len) */
    const void** rp;       /* row pointers (double* or float*); owned array */
    double* buf;           /* widened copies of rows that needed it, or NULL */
    bool    is_vec;
    bool    f32;           /* rows are float (only when requested and all F32) */
} lin_t;

static bool lin_num(ray_t* v) {
    return v && ray_is_vec(v) &&
           (v->type == RAY_F64 || v->type == RAY_F32 ||
            v->type == RAY_I32 || v->type == RAY_I64);
}

/* Every row of x is F32 (x a vector or a non-empty list of vectors). */
static bool lin_all_f32(ray_t* x) {
    if (!x) return false;
    if (ray_is_vec(x)) return x->type == RAY_F32;
    if (x->type != RAY_LIST || x->len <= 0) return false;
    for (int64_t i = 0; i < x->len; i++) {
        ray_t* e = ray_list_get(x, i);
        if (!e || !ray_is_vec(e) || e->type != RAY_F32) return false;
    }
    return true;
}

static void lin_widen(ray_t* v, double* dst) {
    int64_t n = v->len;
    void* s = ray_data(v);
    switch (v->type) {
        case RAY_F32: for (int64_t i = 0; i < n; i++) dst[i] = ((float*)s)[i]; break;
        case RAY_I32: for (int64_t i = 0; i < n; i++) dst[i] = ((int32_t*)s)[i]; break;
        default:      for (int64_t i = 0; i < n; i++) dst[i] = (double)((int64_t*)s)[i]; break;
    }
}

static void lin_free(lin_t* m) {
    if (m->rp) ray_free_raw((void*)m->rp);
    if (m->buf) ray_free_raw(m->buf);
    m->rp = NULL; m->buf = NULL;
}

/* Bind the rows without copying those already of the working type. */
static ray_t* lin_bind(lin_t* m, ray_t* const* rows, int64_t r, int64_t c) {
    uint8_t want = m->f32 ? RAY_F32 : RAY_F64;
    m->rp = (const void**)ray_calloc_raw((size_t)r * sizeof(void*));
    if (!m->rp) return ray_error("oom", NULL);
    int64_t nw = 0;
    for (int64_t i = 0; i < r; i++) if (rows[i]->type != want) nw++;
    if (nw) {
        m->buf = (double*)ray_calloc_raw((size_t)nw * (size_t)c * sizeof(double));
        if (!m->buf) { lin_free(m); return ray_error("oom", NULL); }
    }
    int64_t w = 0;
    for (int64_t i = 0; i < r; i++) {
        if (rows[i]->type == want) m->rp[i] = ray_data(rows[i]);
        else { lin_widen(rows[i], m->buf + w * c); m->rp[i] = m->buf + w * c; w++; }
    }
    return NULL;
}

/* Load an operand.  f32 asks for float rows (the caller has checked that all
 * of them are F32 for the call); otherwise everything is double. */
static ray_t* lin_load(const char* op, ray_t* x, lin_t* m, bool f32) {
    memset(m, 0, sizeof *m);
    m->f32 = f32;
    if (!x) return ray_error("type", "%s: missing operand", op);
    if (lin_num(x)) {
        m->r = 1; m->c = x->len; m->is_vec = true;
        return lin_bind(m, &x, 1, x->len);
    }
    if (x->type != RAY_LIST || x->len <= 0)
        return ray_error("type", "%s: expects a numeric vector or a non-empty list of numeric rows", op);
    ray_t* f = ray_list_get(x, 0);
    if (!lin_num(f) || f->len <= 0)
        return ray_error("type", "%s: matrix rows must be non-empty numeric vectors", op);
    int64_t c = f->len;
    for (int64_t i = 1; i < x->len; i++) {
        ray_t* e = ray_list_get(x, i);
        if (!lin_num(e)) return ray_error("type", "%s: matrix rows must be numeric vectors", op);
        if (e->len != c) return ray_error("length", "%s: matrix rows must have equal length", op);
    }
    m->r = x->len; m->c = c;
    return lin_bind(m, (ray_t* const*)ray_data(x), x->len, c);
}

/* Dense row-major double copy of a double-bound matrix. */
static double* lin_flat(const lin_t* m) {
    double* d = (double*)ray_calloc_raw((size_t)m->r * (size_t)m->c * sizeof(double));
    if (!d) return NULL;
    for (int64_t i = 0; i < m->r; i++)
        memcpy(d + i * m->c, m->rp[i], (size_t)m->c * sizeof(double));
    return d;
}

static ray_t* lin_vec_out(const double* d, int64_t n, bool f32) {
    ray_t* v = ray_vec_new(f32 ? RAY_F32 : RAY_F64, n);
    if (!v || RAY_IS_ERR(v)) return ray_error("oom", NULL);
    v->len = n;
    if (f32) {
        float* o = (float*)ray_data(v);
        for (int64_t i = 0; i < n; i++) {
            double x = d[i];
            if (__builtin_fabs(x) <= DBL_MAX) o[i] = (float)x;
            else { o[i] = NULL_F32; v->attrs |= RAY_ATTR_HAS_NULLS; }
        }
    } else {
        double* o = (double*)ray_data(v);
        for (int64_t i = 0; i < n; i++) o[i] = ray_f64_fin(d[i]);
        mark_f64_nonfinite_as_null(v, 0, n);
    }
    return v;
}

static ray_t* lin_out(const double* d, int64_t r, int64_t c, bool f32) {
    ray_t* L = ray_list_new(r);
    if (!L || RAY_IS_ERR(L)) return ray_error("oom", NULL);
    for (int64_t i = 0; i < r; i++) {
        ray_t* row = lin_vec_out(d + i * c, c, f32);
        if (RAY_IS_ERR(row)) { ray_release(L); return row; }
        L = ray_list_append(L, row);
        ray_release(row);
        if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    }
    return L;
}

/* ---- matrix product kernels, one instantiation per element type ----------
 *
 * C (r x c, double) += A[:, k0:k1] * B[k0:k1, :].  Three shapes:
 *   B a vector      row dot products
 *   r*c small       shared dimension outermost: A and B stream once and the
 *                   accumulator stays in L1 (the X'X case)
 *   otherwise       ikj, B and C accessed along rows
 * dot2 and the small case share one rule: the k loop is the long one. */

#define LIN_SMALL_C 4096
#define LIN_DOT_BLOCK 2048

/* The dot product lives in its own function: inlined into the row loop the
 * vectoriser ran across the rows instead, gathering from the row pointers
 * (several times slower).  A plain loop is what it turns into SIMD best. */
#define DEF_DOT(SUF, T)                                                       \
static __attribute__((noinline)) double dot_##SUF(const T* a, const T* b, int64_t n) { \
    double s = 0.0;                                                           \
    for (int64_t l = 0; l < n; l++) s += (double)a[l] * (double)b[l];         \
    return s;                                                                 \
}
DEF_DOT(f64, double)
DEF_DOT(f32, float)

#define DEF_GEMM(SUF, T)                                                      \
static void gemm_k_##SUF(const lin_t* A, const lin_t* B, double* C,           \
                         int64_t r, int64_t c, int64_t k0, int64_t k1) {      \
    if (B->is_vec) {                                                          \
        const T* v = (const T*)B->rp[0];                                      \
        /* a block of v stays in L1 while every row of A passes over it, so   \
         * v is read from memory once rather than once per row */             \
        for (int64_t b0 = k0; b0 < k1; b0 += LIN_DOT_BLOCK) {                 \
            int64_t b1 = b0 + LIN_DOT_BLOCK < k1 ? b0 + LIN_DOT_BLOCK : k1;   \
            for (int64_t i = 0; i < r; i++) {                                 \
                const T* Ai = (const T*)A->rp[i];                             \
                C[i] += dot_##SUF(Ai + b0, v + b0, b1 - b0);                   \
            }                                                                 \
        }                                                                     \
    } else if (r * c <= LIN_SMALL_C) {                                        \
        for (int64_t l = k0; l < k1; l++) {                                   \
            const T* Bl = (const T*)B->rp[l];                                 \
            for (int64_t i = 0; i < r; i++) {                                 \
                double a = (double)((const T*)A->rp[i])[l];                   \
                double* Ci = C + i * c;                                       \
                for (int64_t j = 0; j < c; j++) Ci[j] += a * (double)Bl[j];   \
            }                                                                 \
        }                                                                     \
    } else {                                                                  \
        for (int64_t i = 0; i < r; i++) {                                     \
            double* Ci = C + i * c;                                           \
            for (int64_t l = k0; l < k1; l++) {                               \
                double a = (double)((const T*)A->rp[i])[l];                   \
                const T* Bl = (const T*)B->rp[l];                             \
                for (int64_t j = 0; j < c; j++) Ci[j] += a * (double)Bl[j];   \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}                                                                             \
static void gemm_rows_##SUF(const lin_t* A, const lin_t* B, double* C,        \
                            int64_t k, int64_t c, int64_t i0, int64_t i1) {   \
    for (int64_t i = i0; i < i1; i++) {                                       \
        double* Ci = C + i * c;                                               \
        for (int64_t l = 0; l < k; l++) {                                     \
            double a = (double)((const T*)A->rp[i])[l];                       \
            const T* Bl = (const T*)B->rp[l];                                 \
            for (int64_t j = 0; j < c; j++) Ci[j] += a * (double)Bl[j];       \
        }                                                                     \
    }                                                                         \
}

DEF_GEMM(f64, double)
DEF_GEMM(f32, float)

typedef struct {
    const lin_t *A, *B;
    double* C;                 /* r x c result (row-split) */
    double* part;              /* kn partial results, r*c each (k-split) */
    int64_t r, k, c, kchunk, rchunk;
    bool    f32, ksplit;
} gemm_ctx_t;

static void gemm_task(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gemm_ctx_t* g = (gemm_ctx_t*)vctx;
    for (int64_t t = start; t < end; t++) {
        if (g->ksplit) {
            int64_t k0 = t * g->kchunk, k1 = k0 + g->kchunk;
            if (k1 > g->k) k1 = g->k;
            double* P = g->part + t * g->r * g->c;
            if (g->f32) gemm_k_f32(g->A, g->B, P, g->r, g->c, k0, k1);
            else        gemm_k_f64(g->A, g->B, P, g->r, g->c, k0, k1);
        } else {
            int64_t i0 = t * g->rchunk, i1 = i0 + g->rchunk;
            if (i1 > g->r) i1 = g->r;
            if (g->f32) gemm_rows_f32(g->A, g->B, g->C, g->k, g->c, i0, i1);
            else        gemm_rows_f64(g->A, g->B, g->C, g->k, g->c, i0, i1);
        }
    }
}

#define LIN_PAR_FLOPS   (1 << 21)   /* below this a dispatch costs more than it saves */
#define LIN_KCHUNK_MIN  2048
#define LIN_MAX_TASKS   256

/* C[r x c] = A[r x k] * B[k x c], C zeroed here.  Returns false on oom. */
static bool lin_gemm(const lin_t* A, const lin_t* B, double* C,
                     int64_t r, int64_t k, int64_t c) {
    memset(C, 0, (size_t)r * (size_t)c * sizeof(double));
    ray_pool_t* pool = ray_pool_get();
    bool par = ray_pool_par_dispatch_ok(pool, k, 1) && r * c * k >= LIN_PAR_FLOPS;
    if (par) {
        gemm_ctx_t g = { .A = A, .B = B, .C = C, .r = r, .k = k, .c = c, .f32 = A->f32 };
        int64_t nw = (int64_t)ray_pool_total_workers(pool);
        g.ksplit = B->is_vec || r * c <= LIN_SMALL_C;
        ray_t* hdr = NULL;
        int64_t nt;
        if (g.ksplit) {
            nt = k / LIN_KCHUNK_MIN;
            if (nt > nw * 2) nt = nw * 2;
            if (nt > LIN_MAX_TASKS) nt = LIN_MAX_TASKS;
            if (nt < 2) par = false;
            else {
                g.kchunk = (k + nt - 1) / nt;
                nt = (k + g.kchunk - 1) / g.kchunk;
                g.part = (double*)ray_calloc_raw((size_t)nt * (size_t)(r * c) * sizeof(double));
                if (!g.part) par = false;
            }
        } else {
            nt = r < nw * 2 ? r : nw * 2;
            if (nt < 2) par = false;
            else { g.rchunk = (r + nt - 1) / nt; nt = (r + g.rchunk - 1) / g.rchunk; }
        }
        (void)hdr;
        if (par) {
            ray_pool_dispatch_n(pool, gemm_task, &g, (uint32_t)nt);
            if (g.ksplit) {
                for (int64_t t = 0; t < nt; t++) {
                    const double* P = g.part + t * r * c;
                    for (int64_t i = 0; i < r * c; i++) C[i] += P[i];
                }
                ray_free_raw(g.part);
            }
            return true;
        }
    }
    if (A->f32) {
        if (B->is_vec || r * c <= LIN_SMALL_C) gemm_k_f32(A, B, C, r, c, 0, k);
        else gemm_rows_f32(A, B, C, k, c, 0, r);
    } else {
        if (B->is_vec || r * c <= LIN_SMALL_C) gemm_k_f64(A, B, C, r, c, 0, k);
        else gemm_rows_f64(A, B, C, k, c, 0, r);
    }
    return true;
}

/* (mmu a b): matrix product.  mat*mat -> mat, mat*vec -> vec,
 * vec*mat -> vec, vec*vec -> atom (dot product, always F64). */
ray_t* ray_mmu_fn(ray_t* a, ray_t* b) {
    lin_t A, B;
    ray_t* e;
    bool f32 = lin_all_f32(a) && lin_all_f32(b);
    if ((e = lin_load("mmu", a, &A, f32))) return e;
    if ((e = lin_load("mmu", b, &B, f32))) { lin_free(&A); return e; }
    ray_t* out;
    /* a vector on the left is a row, on the right a column */
    int64_t ar = A.r, ac = A.c, br = B.r, bc = B.c;
    if (B.is_vec) { br = B.c; bc = 1; }
    if (ac != br) {
        out = ray_error("length", "mmu: shape mismatch, %lld columns vs %lld rows",
                        (long long)ac, (long long)br);
    } else {
        double* C = (double*)ray_calloc_raw((size_t)ar * (size_t)bc * sizeof(double) + 8);
        if (!C) {
            out = ray_error("oom", NULL);
        } else {
            lin_gemm(&A, &B, C, ar, ac, bc);
            if (A.is_vec && B.is_vec) out = make_f64(ray_f64_fin(C[0]));
            else if (A.is_vec || B.is_vec) out = lin_vec_out(C, A.is_vec ? bc : ar, f32);
            else out = lin_out(C, ar, bc, f32);
            ray_free_raw(C);
        }
    }
    lin_free(&A); lin_free(&B);
    return out;
}
/* ---- (gram x): x mmu flip x without the transpose --------------------------
 * G[i][j] = dot(row i, row j).  The matrix is symmetric, so only j >= i is
 * computed (and mirrored).  Rows are cut into blocks of LIN_DOT_BLOCK columns
 * so that all rows' current block stays in cache while the pairs are formed. */

/* 4x4 tile of dot products: G[i0..i0+3][j0..j0+3] over one block of columns.
 * The 16 running sums live in registers (4-lane vectors), so each step costs
 * 8 loads for 16 multiply-adds instead of 2 loads per multiply-add.  Written
 * with vector extensions: the autovectoriser does not find this shape. */
typedef double lin_v4d __attribute__((vector_size(32), aligned(8)));
typedef float  lin_v4f __attribute__((vector_size(16), aligned(4)));
static inline lin_v4d ld4_f64(const double* p) { return *(const lin_v4d*)p; }
static inline lin_v4d ld4_f32(const float* p)  { return __builtin_convertvector(*(const lin_v4f*)p, lin_v4d); }

#define DEF_TILE(SUF, T)                                                      \
static __attribute__((noinline)) void tile4_##SUF(const T* const* a, const T* const* b, \
                                                  int64_t n, double out[16]) { \
    lin_v4d s[4][4];                                                          \
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) s[x][y] = (lin_v4d){0, 0, 0, 0}; \
    int64_t l = 0;                                                            \
    for (; l + 4 <= n; l += 4) {                                              \
        lin_v4d av[4], bv[4];                                                 \
        for (int x = 0; x < 4; x++) { av[x] = ld4_##SUF(a[x] + l); bv[x] = ld4_##SUF(b[x] + l); } \
        for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) s[x][y] += av[x] * bv[y]; \
    }                                                                         \
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) {                 \
        lin_v4d v = s[x][y];                                                  \
        double t = (v[0] + v[1]) + (v[2] + v[3]);                             \
        for (int64_t q = l; q < n; q++) t += (double)a[x][q] * (double)b[y][q]; \
        out[x * 4 + y] = t;                                                   \
    }                                                                         \
}
DEF_TILE(f64, double)
DEF_TILE(f32, float)

#define DEF_GRAM(SUF, T)                                                      \
static void gram_k_##SUF(const lin_t* A, double* G, int64_t p,                \
                         int64_t k0, int64_t k1) {                            \
    int64_t p4 = p & ~(int64_t)3;                                             \
    for (int64_t b0 = k0; b0 < k1; b0 += LIN_DOT_BLOCK) {                     \
        int64_t b1 = b0 + LIN_DOT_BLOCK < k1 ? b0 + LIN_DOT_BLOCK : k1;       \
        int64_t n = b1 - b0;                                                  \
        for (int64_t i = 0; i < p4; i += 4) {                                 \
            const T* ra[4];                                                   \
            for (int x = 0; x < 4; x++) ra[x] = (const T*)A->rp[i + x] + b0;  \
            for (int64_t j = i; j < p4; j += 4) {                             \
                const T* rb[4];                                               \
                for (int x = 0; x < 4; x++) rb[x] = (const T*)A->rp[j + x] + b0; \
                double t[16];                                                 \
                tile4_##SUF(ra, rb, n, t);                                    \
                for (int x = 0; x < 4; x++)                                   \
                    for (int y = 0; y < 4; y++)                               \
                        if (j + y >= i + x) G[(i + x) * p + j + y] += t[x * 4 + y]; \
            }                                                                 \
            /* columns past the last full tile */                             \
            for (int64_t j = p4; j < p; j++)                                  \
                for (int x = 0; x < 4; x++)                                   \
                    G[(i + x) * p + j] += dot_##SUF(ra[x], (const T*)A->rp[j] + b0, n); \
        }                                                                     \
        for (int64_t i = p4; i < p; i++) {                                    \
            const T* Ai = (const T*)A->rp[i] + b0;                            \
            for (int64_t j = i; j < p; j++)                                   \
                G[i * p + j] += dot_##SUF(Ai, (const T*)A->rp[j] + b0, n);    \
        }                                                                     \
    }                                                                         \
}
DEF_GRAM(f64, double)
DEF_GRAM(f32, float)

typedef struct {
    const lin_t* A;
    double* part;                 /* nt partial p x p results */
    int64_t p, k, kchunk;
} gram_ctx_t;

static void gram_task(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gram_ctx_t* g = (gram_ctx_t*)vctx;
    for (int64_t t = start; t < end; t++) {
        int64_t k0 = t * g->kchunk, k1 = k0 + g->kchunk;
        if (k1 > g->k) k1 = g->k;
        double* P = g->part + t * g->p * g->p;
        if (g->A->f32) gram_k_f32(g->A, P, g->p, k0, k1);
        else           gram_k_f64(g->A, P, g->p, k0, k1);
    }
}

/* G[p x p] (zeroed here, upper triangle then mirrored) from A (p rows of k). */
static void lin_gram(const lin_t* A, double* G, int64_t p, int64_t k) {
    memset(G, 0, (size_t)p * (size_t)p * sizeof(double));
    ray_pool_t* pool = ray_pool_get();
    bool par = ray_pool_par_dispatch_ok(pool, k, 1) && p * p * k >= LIN_PAR_FLOPS;
    int64_t nt = 0;
    gram_ctx_t g = { .A = A, .part = NULL, .p = p, .k = k, .kchunk = 0 };
    if (par) {
        int64_t nw = (int64_t)ray_pool_total_workers(pool);
        nt = k / LIN_KCHUNK_MIN;
        if (nt > nw * 2) nt = nw * 2;
        if (nt > LIN_MAX_TASKS) nt = LIN_MAX_TASKS;
        /* keep the partial buffers modest for wide matrices */
        while (nt > 1 && nt * p * p > (1 << 23)) nt--;
        if (nt < 2) par = false;
        else {
            g.kchunk = (k + nt - 1) / nt;
            nt = (k + g.kchunk - 1) / g.kchunk;
            g.part = (double*)ray_calloc_raw((size_t)nt * (size_t)(p * p) * sizeof(double));
            if (!g.part) par = false;
        }
    }
    if (par) {
        ray_pool_dispatch_n(pool, gram_task, &g, (uint32_t)nt);
        for (int64_t t = 0; t < nt; t++) {
            const double* P = g.part + t * p * p;
            for (int64_t i = 0; i < p * p; i++) G[i] += P[i];
        }
        ray_free_raw(g.part);
    } else if (A->f32) gram_k_f32(A, G, p, 0, k);
    else               gram_k_f64(A, G, p, 0, k);
    for (int64_t i = 0; i < p; i++)
        for (int64_t j = 0; j < i; j++) G[i * p + j] = G[j * p + i];
}

/* (gram x): x mmu flip x, for x a matrix (list of rows). */
ray_t* ray_gram_fn(ray_t* x) {
    lin_t A;
    ray_t* e;
    bool f32 = lin_all_f32(x);
    if ((e = lin_load("gram", x, &A, f32))) return e;
    if (A.is_vec) { lin_free(&A); return ray_error("type", "gram: expects a matrix (list of numeric rows)"); }
    ray_t* out;
    double* G = (double*)ray_calloc_raw((size_t)A.r * (size_t)A.r * sizeof(double));
    if (!G) out = ray_error("oom", NULL);
    else {
        lin_gram(&A, G, A.r, A.c);
        out = lin_out(G, A.r, A.r, f32);
        ray_free_raw(G);
    }
    lin_free(&A);
    return out;
}

/* In-place Gauss-Jordan with partial pivoting; a is n x n, inv gets A^-1.
 * Returns false when the matrix is singular. */
static bool lin_invert(double* a, double* inv, int64_t n) {
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++) inv[i * n + j] = (i == j);
    for (int64_t k = 0; k < n; k++) {
        int64_t p = k;
        double best = fabs(a[k * n + k]);
        for (int64_t i = k + 1; i < n; i++)
            if (fabs(a[i * n + k]) > best) { best = fabs(a[i * n + k]); p = i; }
        if (!(best > 0.0) || !isfinite(best)) return false;
        if (p != k)
            for (int64_t j = 0; j < n; j++) {
                double t = a[k * n + j]; a[k * n + j] = a[p * n + j]; a[p * n + j] = t;
                t = inv[k * n + j]; inv[k * n + j] = inv[p * n + j]; inv[p * n + j] = t;
            }
        double s = 1.0 / a[k * n + k];
        for (int64_t j = 0; j < n; j++) { a[k * n + j] *= s; inv[k * n + j] *= s; }
        for (int64_t i = 0; i < n; i++) {
            if (i == k) continue;
            double f = a[i * n + k];
            if (f == 0.0) continue;
            for (int64_t j = 0; j < n; j++) {
                a[i * n + j]   -= f * a[k * n + j];
                inv[i * n + j] -= f * inv[k * n + j];
            }
        }
    }
    return true;
}

/* (inv m): inverse of a square matrix. */
ray_t* ray_inv_fn(ray_t* x) {
    lin_t A;
    ray_t* e;
    if ((e = lin_load("inv", x, &A, false))) return e;
    ray_t* out;
    if (A.is_vec || A.r != A.c) {
        out = ray_error("length", "inv: expects a square matrix");
    } else {
        int64_t n = A.r;
        double* I = (double*)ray_calloc_raw((size_t)n * (size_t)n * sizeof(double));
        double* W = lin_flat(&A);
        if (!I || !W) out = ray_error("oom", NULL);
        else out = lin_invert(W, I, n) ? lin_out(I, n, n, lin_all_f32(x))
                                       : ray_error("domain", "inv: singular matrix");
        if (I) ray_free_raw(I);
        if (W) ray_free_raw(W);
    }
    lin_free(&A);
    return out;
}

/* Least squares by Householder QR.  Solves min ||M w - R||_F column by
 * column for M (m x n, m >= n) and R (m x k); w (n x k) is written to W.
 * Returns false when M is rank deficient. */
static bool lin_qr_solve(double* M, double* R, double* W, int64_t m, int64_t n, int64_t k) {
    double* diag = (double*)ray_calloc_raw((size_t)n * sizeof(double));
    if (!diag) return false;
    bool ok = true;
    for (int64_t j = 0; j < n && ok; j++) {
        double nrm = 0.0;
        for (int64_t i = j; i < m; i++) nrm += M[i * n + j] * M[i * n + j];
        nrm = sqrt(nrm);
        if (!(nrm > 0.0) || !isfinite(nrm)) { ok = false; break; }
        if (M[j * n + j] > 0.0) nrm = -nrm;
        /* v = x - nrm*e1 stored in column j (below and on the diagonal) */
        M[j * n + j] -= nrm;
        double vv = 0.0;
        for (int64_t i = j; i < m; i++) vv += M[i * n + j] * M[i * n + j];
        diag[j] = nrm;
        for (int64_t c = j + 1; c < n; c++) {
            double s = 0.0;
            for (int64_t i = j; i < m; i++) s += M[i * n + j] * M[i * n + c];
            s = 2.0 * s / vv;
            for (int64_t i = j; i < m; i++) M[i * n + c] -= s * M[i * n + j];
        }
        for (int64_t c = 0; c < k; c++) {
            double s = 0.0;
            for (int64_t i = j; i < m; i++) s += M[i * n + j] * R[i * k + c];
            s = 2.0 * s / vv;
            for (int64_t i = j; i < m; i++) R[i * k + c] -= s * M[i * n + j];
        }
    }
    if (ok) {
        /* the R factor: strict upper part in M, diagonal in diag */
        double tol = 0.0;
        for (int64_t j = 0; j < n; j++) tol = fmax(tol, fabs(diag[j]));
        tol *= 1e-13 * (double)n;
        for (int64_t j = 0; j < n; j++) if (fabs(diag[j]) <= tol) { ok = false; break; }
    }
    if (ok) {
        for (int64_t c = 0; c < k; c++)
            for (int64_t i = n - 1; i >= 0; i--) {
                double s = R[i * k + c];
                for (int64_t j = i + 1; j < n; j++) s -= M[i * n + j] * W[j * k + c];
                W[i * k + c] = s / diag[i];
            }
    }
    ray_free_raw(diag);
    return ok;
}

/* y[0..n) -= f * x[0..n): its own function so it vectorises as a plain loop. */
static __attribute__((noinline)) void lin_axpy(double* y, const double* x, double f, int64_t n) {
    for (int64_t j = 0; j < n; j++) y[j] -= f * x[j];
}

/* Square solve M W = R by Gaussian elimination with partial pivoting; M is
 * n x n, R and the result W are n x k (R is overwritten).  About a third of
 * the work of forming the inverse, and no inverse is formed.  Returns false
 * when M is singular. */
static bool lin_lu_solve(double* M, double* R, double* W, int64_t n, int64_t k) {
    for (int64_t c = 0; c < n; c++) {
        int64_t p = c;
        double best = fabs(M[c * n + c]);
        for (int64_t i = c + 1; i < n; i++)
            if (fabs(M[i * n + c]) > best) { best = fabs(M[i * n + c]); p = i; }
        if (!(best > 0.0) || !isfinite(best)) return false;
        if (p != c) {
            for (int64_t j = 0; j < n; j++) { double t = M[c * n + j]; M[c * n + j] = M[p * n + j]; M[p * n + j] = t; }
            for (int64_t j = 0; j < k; j++) { double t = R[c * k + j]; R[c * k + j] = R[p * k + j]; R[p * k + j] = t; }
        }
        double piv = M[c * n + c];
        for (int64_t i = c + 1; i < n; i++) {
            double f = M[i * n + c] / piv;
            if (f == 0.0) continue;
            lin_axpy(M + i * n + c + 1, M + c * n + c + 1, f, n - c - 1);
            lin_axpy(R + i * k, R + c * k, f, k);
        }
    }
    for (int64_t i = n - 1; i >= 0; i--) {
        for (int64_t j = i + 1; j < n; j++)
            lin_axpy(R + i * k, R + j * k, M[i * n + j], k);
        double d = M[i * n + i];
        for (int64_t j = 0; j < k; j++) { R[i * k + j] /= d; W[i * k + j] = R[i * k + j]; }
    }
    return true;
}

/* (lsq b a): the matrix Z minimising ||Z a - b||, i.e. b mmu inv a when a is
 * square and the least-squares generalisation when a has more columns than
 * rows.  A vector b is one row and gives a vector. */
ray_t* ray_lsq_fn(ray_t* b, ray_t* a) {
    lin_t B, A;
    ray_t* e;
    if ((e = lin_load("lsq", b, &B, false))) return e;
    if ((e = lin_load("lsq", a, &A, false))) { lin_free(&B); return e; }
    ray_t* out = NULL;
    int64_t n = A.is_vec ? A.c : A.r;       /* unknowns per row of Z */
    int64_t m = A.is_vec ? 1 : A.c;         /* equations */
    if (A.is_vec) { out = ray_error("type", "lsq: right operand must be a matrix"); goto done; }
    if (B.c != m) { out = ray_error("length", "lsq: shape mismatch, %lld columns vs %lld", (long long)B.c, (long long)m); goto done; }
    if (m < n) { out = ray_error("length", "lsq: system is under-determined"); goto done; }
    {
        int64_t k = B.r;
        /* Z a = b  <=>  a^T Z^T = b^T: M = a^T (m x n), R = b^T (m x k) */
        double* M = (double*)ray_calloc_raw((size_t)m * (size_t)n * sizeof(double));
        double* R = (double*)ray_calloc_raw((size_t)m * (size_t)k * sizeof(double));
        double* W = (double*)ray_calloc_raw((size_t)n * (size_t)k * sizeof(double));
        if (!M || !R || !W) out = ray_error("oom", NULL);
        else {
            for (int64_t i = 0; i < n; i++)
                for (int64_t j = 0; j < m; j++) M[j * n + i] = ((const double*)A.rp[i])[j];
            for (int64_t i = 0; i < k; i++)
                for (int64_t j = 0; j < m; j++) R[j * k + i] = ((const double*)B.rp[i])[j];
            if (!(m == n ? lin_lu_solve(M, R, W, n, k) : lin_qr_solve(M, R, W, m, n, k))) out = ray_error("domain", "lsq: rank-deficient matrix");
            else {
                /* Z = W^T (k x n) */
                double* Z = (double*)ray_calloc_raw((size_t)k * (size_t)n * sizeof(double));
                if (!Z) out = ray_error("oom", NULL);
                else {
                    for (int64_t i = 0; i < k; i++)
                        for (int64_t j = 0; j < n; j++) Z[i * n + j] = W[j * k + i];
                    { bool f = lin_all_f32(a) && lin_all_f32(b);
                      out = B.is_vec ? lin_vec_out(Z, n, f) : lin_out(Z, k, n, f); }
                    ray_free_raw(Z);
                }
            }
        }
        if (M) ray_free_raw(M);
        if (R) ray_free_raw(R);
        if (W) ray_free_raw(W);
    }
done:
    lin_free(&A); lin_free(&B);
    return out;
}

/* (flip m): transpose of a matrix (list of rows -> list of columns). */
ray_t* ray_flip_mat_fn(ray_t* x) {
    lin_t A;
    ray_t* e;
    if ((e = lin_load("flip", x, &A, false))) return e;
    if (A.is_vec) { lin_free(&A); return ray_error("type", "flip: expects a matrix (list of numeric rows)"); }
    double* T = (double*)ray_calloc_raw((size_t)A.r * (size_t)A.c * sizeof(double));
    ray_t* out;
    if (!T) out = ray_error("oom", NULL);
    else {
        for (int64_t i = 0; i < A.r; i++)
            for (int64_t j = 0; j < A.c; j++) T[j * A.r + i] = ((const double*)A.rp[i])[j];
        out = lin_out(T, A.c, A.r, lin_all_f32(x));
        ray_free_raw(T);
    }
    lin_free(&A);
    return out;
}
