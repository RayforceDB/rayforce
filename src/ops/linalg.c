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
 *  Dense linear algebra builtins: mmu, inv, lsq, flip, gram
 *
 *  A matrix is a RAY_LIST of equal-length numeric vectors, one per row; a
 *  bare numeric vector is a vector operand.  F64 and F32 rows are used in
 *  place; I32/I64 rows are widened to double, their nulls becoming NaN (the
 *  F64 null), so integer rows behave like F64 rows.
 *
 *  Result type: F32 when every operand is F32, else F64.  The products mmu
 *  and gram are computed in the operands' own precision: F32 rows multiply
 *  and sum in float, eight to a register where F64 has four, and F64 rows in
 *  double.  Whoever passes F32 has chosen single precision.  The solvers inv
 *  and lsq always work in double, whatever they are given: elimination loses
 *  digits to conditioning that a product does not.
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
        case RAY_I32:
            for (int64_t i = 0; i < n; i++) {
                int32_t x = ((int32_t*)s)[i];
                dst[i] = x == NULL_I32 ? NULL_F64 : (double)x;
            }
            break;
        default:
            for (int64_t i = 0; i < n; i++) {
                int64_t x = ((int64_t*)s)[i];
                dst[i] = x == NULL_I64 ? NULL_F64 : (double)x;
            }
            break;
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

/* True when any element of a double-bound matrix is a null or not finite.
 * inv and lsq refuse such input: a null is missing data, not a singularity. */
static bool lin_nonfinite(const lin_t* m) {
    for (int64_t i = 0; i < m->r; i++) {
        const double* p = (const double*)m->rp[i];
        for (int64_t j = 0; j < m->c; j++) if (!isfinite(p[j])) return true;
    }
    return false;
}

/* Largest absolute element of a row-major n x n (or r x c) double array. */
static double lin_maxabs(const double* a, int64_t cnt) {
    double mx = 0.0;
    for (int64_t i = 0; i < cnt; i++) mx = fmax(mx, fabs(a[i]));
    return mx;
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
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    v->len = n;
    if (f32) {
        float* o = (float*)ray_data(v);
        for (int64_t i = 0; i < n; i++) {
            double x = d[i];
            if (__builtin_fabs(x) <= (double)FLT_MAX) o[i] = (float)x;
            else { o[i] = NULL_F32; v->attrs |= RAY_ATTR_HAS_NULLS; }
        }
    } else {
        double* o = (double*)ray_data(v);
        for (int64_t i = 0; i < n; i++) o[i] = ray_f64_fin(d[i]);
        mark_f64_nonfinite_as_null(v, 0, n);
    }
    return v;
}

/* An F32 vector from float results; a value that is not finite is a null. */
static ray_t* lin_vec_out_f32(const float* d, int64_t n) {
    ray_t* v = ray_vec_new(RAY_F32, n);
    if (!v || RAY_IS_ERR(v)) return v ? v : ray_error("oom", NULL);
    v->len = n;
    float* o = (float*)ray_data(v);
    for (int64_t i = 0; i < n; i++) {
        if (__builtin_fabsf(d[i]) <= FLT_MAX) o[i] = d[i];
        else { o[i] = NULL_F32; v->attrs |= RAY_ATTR_HAS_NULLS; }
    }
    return v;
}

static ray_t* lin_out_f32(const float* d, int64_t r, int64_t c) {
    ray_t* L = ray_list_new(r);
    if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    for (int64_t i = 0; i < r; i++) {
        ray_t* row = lin_vec_out_f32(d + i * c, c);
        if (RAY_IS_ERR(row)) { ray_release(L); return row; }
        L = ray_list_append_take(L, row);
        ray_release(row);
        if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    }
    return L;
}

static ray_t* lin_out(const double* d, int64_t r, int64_t c, bool f32) {
    ray_t* L = ray_list_new(r);
    if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    for (int64_t i = 0; i < r; i++) {
        ray_t* row = lin_vec_out(d + i * c, c, f32);
        if (RAY_IS_ERR(row)) { ray_release(L); return row; }
        L = ray_list_append_take(L, row);
        ray_release(row);
        if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    }
    return L;
}

/* ---- matrix product kernels, one instantiation per element type ----------
 *
 * C (r x c, of the operands' type) += A[:, k0:k1] * B[k0:k1, :].  Three shapes:
 *   B a vector      row dot products
 *   r*c small       shared dimension outermost: A and B stream once and the
 *                   accumulator stays in L1 (the X'X case)
 *   otherwise       ikj over a tile of columns: LIN_JTILE entries of a row
 *                   of C stay in L1 while four rows of B at a time are added
 *                   into it, so C is loaded and stored once for four
 *                   multiply-adds instead of once for each
 * dot2 and the small case share one rule: the k loop is the long one. */

#define LIN_JTILE 256

#define LIN_SMALL_C 4096
#define LIN_DOT_BLOCK 2048

/* The dot product lives in its own function: inlined into the row loop the
 * vectoriser ran across the rows instead, gathering from the row pointers
 * (several times slower).  A plain loop is what it turns into SIMD best. */
#define DEF_DOT(SUF, T)                                                       \
static __attribute__((noinline)) T dot_##SUF(const T* a, const T* b, int64_t n) { \
    T s = 0;                                                                  \
    for (int64_t l = 0; l < n; l++) s += a[l] * b[l];                         \
    return s;                                                                 \
}
DEF_DOT(f64, double)
DEF_DOT(f32, float)

/* Vector types of the rows' own width (four doubles, or eight floats), for
 * the kernels the autovectoriser does not find the shape of. */
typedef double lin_v4d __attribute__((vector_size(32), aligned(8)));
typedef float  lin_v8f __attribute__((vector_size(32), aligned(4)));
/* Macros, not functions: a 32-byte vector passed or returned by value changes
 * the ABI without AVX (-Wpsabi, an error under -Werror on baseline x86-64). */
#define ld_f64(p) (*(const lin_v4d*)(p))
#define ld_f32(p) (*(const lin_v8f*)(p))

/* ---- the product's kernel -------------------------------------------------
 *
 * C += A * B with a block of C held in registers: four rows by three vectors
 * (24 floats, or 12 doubles).  For each step along the shared dimension the
 * block takes one number from each of its four rows of A and three vectors
 * of B: twelve multiply-adds for three loads, and nothing of C is loaded or
 * stored inside the loop.  Twelve sums, three vectors of B and one of A are
 * the sixteen registers AVX2 has.
 *
 * B is not read where it lies.  The strip of it under the block's columns is
 * packed first, LIN_KC steps of the shared dimension at a time, into a block
 * of its own that stays in L1 while every row of A passes over it; the copy
 * is one pass for a strip that is then used once for each four rows.  Packing
 * is also what lets B lie either way: as rows [k x n], or on its side as
 * [n x k], which is how a model's file holds a layer's weights. */
#define LIN_KC 256

#define DEF_MICRO(SUF, T, V, W)                                               \
static inline __attribute__((always_inline)) void                             \
micro_##SUF(const T* const* a, const int rows, const T* restrict p,           \
            const int nv, int64_t kb, T* c, int64_t cs) {                     \
    V acc[4][3];                                                              \
    for (int x = 0; x < rows; x++)                                            \
        for (int y = 0; y < nv; y++) acc[x][y] = ld_##SUF(c + x * cs + y * W); \
    for (int64_t l = 0; l < kb; l++) {                                        \
        V b[3];                                                               \
        for (int y = 0; y < nv; y++) b[y] = ld_##SUF(p + l * nv * W + y * W); \
        for (int x = 0; x < rows; x++) {                                      \
            V s = (V){0} + a[x][l];                                           \
            for (int y = 0; y < nv; y++) acc[x][y] += s * b[y];               \
        }                                                                     \
    }                                                                         \
    for (int x = 0; x < rows; x++)                                            \
        for (int y = 0; y < nv; y++) *(V*)(c + x * cs + y * W) = acc[x][y];   \
}                                                                             \
/* Out of line, one for each shape, so that each is compiled for its own. */  \
static __attribute__((noinline)) void micro43_##SUF(const T* const* a, const T* p, int64_t kb, T* c, int64_t cs) { micro_##SUF(a, 4, p, 3, kb, c, cs); } \
static __attribute__((noinline)) void micro42_##SUF(const T* const* a, const T* p, int64_t kb, T* c, int64_t cs) { micro_##SUF(a, 4, p, 2, kb, c, cs); } \
static __attribute__((noinline)) void micro41_##SUF(const T* const* a, const T* p, int64_t kb, T* c, int64_t cs) { micro_##SUF(a, 4, p, 1, kb, c, cs); } \
static __attribute__((noinline)) void micro13_##SUF(const T* const* a, const T* p, int64_t kb, T* c, int64_t cs) { micro_##SUF(a, 1, p, 3, kb, c, cs); } \
static __attribute__((noinline)) void micro12_##SUF(const T* const* a, const T* p, int64_t kb, T* c, int64_t cs) { micro_##SUF(a, 1, p, 2, kb, c, cs); } \
static __attribute__((noinline)) void micro11_##SUF(const T* const* a, const T* p, int64_t kb, T* c, int64_t cs) { micro_##SUF(a, 1, p, 1, kb, c, cs); } \
/* Columns [j0, j1) of C[m x n] += A[m x k] * B.  A is m rows, by pointer     \
 * (ap) or `as` apart from a; C's rows are cs apart.  B is k rows of n, or    \
 * with `side` n rows of k; by pointer (bp) or `bs` apart from b.  Blocks     \
 * are laid from j0 and from the first row, so a caller that splits the       \
 * columns among workers splits at multiples of three vectors, and one that   \
 * splits the rows at multiples of four, to have each number computed one     \
 * way whatever the split. */                                                 \
static void lin_mul_##SUF(const T* const* ap, const T* a, int64_t as, int64_t m, int64_t k, \
                          const T* const* bp, const T* b, int64_t bs, bool side, \
                          T* c, int64_t cs, int64_t j0, int64_t j1) {         \
    T pack[LIN_KC * 3 * W];                                                   \
    int64_t jv = j0 + (j1 - j0) / W * W;                                      \
    for (int64_t l0 = 0; l0 < k; l0 += LIN_KC) {                              \
        int64_t kb = l0 + LIN_KC < k ? LIN_KC : k - l0;                       \
        for (int64_t j = j0; j < jv;) {                                       \
            int nv = (jv - j) / W >= 3 ? 3 : (int)((jv - j) / W);             \
            int64_t sw = (int64_t)nv * W;                                     \
            if (side) {                                                       \
                for (int64_t jj = 0; jj < sw; jj++) {                         \
                    const T* wr = (bp ? bp[j + jj] : b + (j + jj) * bs) + l0; \
                    for (int64_t l = 0; l < kb; l++) pack[l * sw + jj] = wr[l]; \
                }                                                             \
            } else {                                                          \
                for (int64_t l = 0; l < kb; l++)                              \
                    memcpy(pack + l * sw, (bp ? bp[l0 + l] : b + (l0 + l) * bs) + j, (size_t)sw * sizeof(T)); \
            }                                                                 \
            int64_t i = 0;                                                    \
            for (; i + 4 <= m; i += 4) {                                      \
                const T* r4[4];                                               \
                for (int x = 0; x < 4; x++) r4[x] = (ap ? ap[i + x] : a + (i + x) * as) + l0; \
                T* ci = c + i * cs + j;                                       \
                if (nv == 3) micro43_##SUF(r4, pack, kb, ci, cs);             \
                else if (nv == 2) micro42_##SUF(r4, pack, kb, ci, cs);        \
                else micro41_##SUF(r4, pack, kb, ci, cs);                     \
            }                                                                 \
            for (; i < m; i++) {                                              \
                const T* r1 = (ap ? ap[i] : a + i * as) + l0;                 \
                T* ci = c + i * cs + j;                                       \
                if (nv == 3) micro13_##SUF(&r1, pack, kb, ci, cs);            \
                else if (nv == 2) micro12_##SUF(&r1, pack, kb, ci, cs);       \
                else micro11_##SUF(&r1, pack, kb, ci, cs);                    \
            }                                                                 \
            j += sw;                                                          \
        }                                                                     \
        /* the columns past the last whole vector, one number at a time */    \
        for (int64_t j = jv; j < j1; j++) {                                   \
            if (side) {                                                       \
                const T* wr = (bp ? bp[j] : b + j * bs) + l0;                 \
                for (int64_t i = 0; i < m; i++)                               \
                    c[i * cs + j] += dot_##SUF((ap ? ap[i] : a + i * as) + l0, wr, kb); \
            } else {                                                          \
                for (int64_t l = 0; l < kb; l++) pack[l] = (bp ? bp[l0 + l] : b + (l0 + l) * bs)[j]; \
                for (int64_t i = 0; i < m; i++)                               \
                    c[i * cs + j] += dot_##SUF((ap ? ap[i] : a + i * as) + l0, pack, kb); \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}
DEF_MICRO(f64, double, lin_v4d, 4)
DEF_MICRO(f32, float,  lin_v8f, 8)

/* The same for callers outside that hold F32 matrices as blocks: columns
 * [j0, j1) of c[m rows, cs apart] += a[m x k, rows `as` apart] * b, b being k
 * rows of n, or with `sideways` n rows of k, `bs` apart.  One thread; split
 * the columns at multiples of RAY_LIN_STRIP_F32. */
void ray_lin_mul_acc_f32(const float* a, int64_t as, int64_t m, int64_t k,
                         const float* b, int64_t bs, bool sideways,
                         float* c, int64_t cs, int64_t j0, int64_t j1) {
    lin_mul_f32(NULL, a, as, m, k, NULL, b, bs, sideways, c, cs, j0, j1);
}

#define DEF_GEMM(SUF, T)                                                      \
static void gemm_k_##SUF(const lin_t* A, const lin_t* B, T* C,                \
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
    } else {                                                                  \
        for (int64_t l = k0; l < k1; l++) {                                   \
            const T* Bl = (const T*)B->rp[l];                                 \
            for (int64_t i = 0; i < r; i++) {                                 \
                T a = ((const T*)A->rp[i])[l];                                \
                T* Ci = C + i * c;                                            \
                for (int64_t j = 0; j < c; j++) Ci[j] += a * Bl[j];           \
            }                                                                 \
        }                                                                     \
    }                                                                         \
}

DEF_GEMM(f64, double)
DEF_GEMM(f32, float)

typedef struct {
    const lin_t *A, *B;
    void* C;                   /* r x c result (row-split), float or double as A is */
    void* part;                /* kn partial results, r*c each (k-split) */
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
            if (g->f32) gemm_k_f32(g->A, g->B, (float*)g->part + t * g->r * g->c, g->r, g->c, k0, k1);
            else        gemm_k_f64(g->A, g->B, (double*)g->part + t * g->r * g->c, g->r, g->c, k0, k1);
        } else {
            int64_t i0 = t * g->rchunk, i1 = i0 + g->rchunk;
            if (i1 > g->r) i1 = g->r;
            if (g->f32) lin_mul_f32((const float* const*)g->A->rp + i0, NULL, 0, i1 - i0, g->k, (const float* const*)g->B->rp, NULL, 0, false,
                                    (float*)g->C + i0 * g->c, g->c, 0, g->c);
            else        lin_mul_f64((const double* const*)g->A->rp + i0, NULL, 0, i1 - i0, g->k, (const double* const*)g->B->rp, NULL, 0, false,
                                    (double*)g->C + i0 * g->c, g->c, 0, g->c);
        }
    }
}

#define LIN_PAR_FLOPS   (1 << 21)   /* below this a dispatch costs more than it saves */
#define LIN_KCHUNK_MIN  2048
#define LIN_MAX_TASKS   256

static bool lin_gemm_run(const lin_t* A, const lin_t* B, void* C,
                         int64_t r, int64_t k, int64_t c, size_t esz);

/* C[r x c] = A[r x k] * B[k x c], C zeroed here: an array of float when A
 * (and so B) is bound as F32, of double otherwise.  Returns false on oom. */
static bool lin_gemm(const lin_t* A, const lin_t* B, void* C,
                     int64_t r, int64_t k, int64_t c) {
    size_t esz = A->f32 ? sizeof(float) : sizeof(double);
    memset(C, 0, (size_t)r * (size_t)c * esz);
    /* The kernel packs the strip of B it works on, so the rows are read
     * where they lie, wherever the allocator put each. */
    return lin_gemm_run(A, B, C, r, k, c, esz);
}

static bool lin_gemm_run(const lin_t* A, const lin_t* B, void* C,
                         int64_t r, int64_t k, int64_t c, size_t esz) {
    ray_pool_t* pool = ray_pool_get();
    bool par = ray_pool_par_dispatch_ok(pool, k, 1) && r * c * k >= LIN_PAR_FLOPS;
    if (par) {
        gemm_ctx_t g = { .A = A, .B = B, .C = C, .r = r, .k = k, .c = c, .f32 = A->f32 };
        int64_t nw = (int64_t)ray_pool_total_workers(pool);
        g.ksplit = B->is_vec || r * c <= LIN_SMALL_C;
        int64_t nt;
        if (g.ksplit) {
            nt = k / LIN_KCHUNK_MIN;
            if (nt > nw * 2) nt = nw * 2;
            if (nt > LIN_MAX_TASKS) nt = LIN_MAX_TASKS;
            if (nt < 2) par = false;
            else {
                g.kchunk = (k + nt - 1) / nt;
                nt = (k + g.kchunk - 1) / g.kchunk;
                g.part = ray_calloc_raw((size_t)nt * (size_t)(r * c) * esz);
                if (!g.part) par = false;
            }
        } else {
            nt = r < nw * 2 ? r : nw * 2;
            if (nt < 2) par = false;
            else {
                /* A multiple of four: the kernel takes rows four at a time
                 * from where a task begins, so a row is computed the same
                 * way whatever the number of workers. */
                g.rchunk = ((r + nt - 1) / nt + 3) & ~(int64_t)3;
                nt = (r + g.rchunk - 1) / g.rchunk;
                if (nt < 2) par = false;
            }
        }
        if (par) {
            ray_pool_dispatch_n(pool, gemm_task, &g, (uint32_t)nt);
            if (g.ksplit) {
                for (int64_t t = 0; t < nt; t++) {
                    if (A->f32) {
                        const float* P = (const float*)g.part + t * r * c;
                        float* Cf = (float*)C;
                        for (int64_t i = 0; i < r * c; i++) Cf[i] += P[i];
                    } else {
                        const double* P = (const double*)g.part + t * r * c;
                        double* Cd = (double*)C;
                        for (int64_t i = 0; i < r * c; i++) Cd[i] += P[i];
                    }
                }
                ray_free_raw(g.part);
            }
            return true;
        }
    }
    if (A->f32) {
        if (B->is_vec || r * c <= LIN_SMALL_C) gemm_k_f32(A, B, (float*)C, r, c, 0, k);
        else lin_mul_f32((const float* const*)A->rp, NULL, 0, r, k, (const float* const*)B->rp, NULL, 0, false, (float*)C, c, 0, c);
    } else {
        if (B->is_vec || r * c <= LIN_SMALL_C) gemm_k_f64(A, B, (double*)C, r, c, 0, k);
        else lin_mul_f64((const double* const*)A->rp, NULL, 0, r, k, (const double* const*)B->rp, NULL, 0, false, (double*)C, c, 0, c);
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
        void* C = ray_calloc_raw((size_t)ar * (size_t)bc * (f32 ? sizeof(float) : sizeof(double)) + 8);
        if (!C) {
            out = ray_error("oom", NULL);
        } else {
            lin_gemm(&A, &B, C, ar, ac, bc);
            int64_t n = A.is_vec ? bc : ar;
            if (f32) {
                const float* Cf = (const float*)C;
                if (A.is_vec && B.is_vec) out = make_f64(ray_f64_fin((double)Cf[0]));
                else if (A.is_vec || B.is_vec) out = lin_vec_out_f32(Cf, n);
                else out = lin_out_f32(Cf, ar, bc);
            } else {
                const double* Cd = (const double*)C;
                if (A.is_vec && B.is_vec) out = make_f64(ray_f64_fin(Cd[0]));
                else if (A.is_vec || B.is_vec) out = lin_vec_out(Cd, n, false);
                else out = lin_out(Cd, ar, bc, false);
            }
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
 * The 16 running sums live in registers, a vector of the rows' own type each
 * (four doubles, or eight floats), so each step costs 8 loads for 16
 * multiply-adds instead of 2 loads per multiply-add.  Written with vector
 * extensions: the autovectoriser does not find this shape. */

#define DEF_TILE(SUF, T, V, W)                                                \
static __attribute__((noinline)) void tile4_##SUF(const T* const* a, const T* const* b, \
                                                  int64_t n, T out[16]) {     \
    V s[4][4];                                                                \
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) s[x][y] = (V){0}; \
    int64_t l = 0;                                                            \
    for (; l + W <= n; l += W) {                                              \
        V av[4], bv[4];                                                       \
        for (int x = 0; x < 4; x++) { av[x] = ld_##SUF(a[x] + l); bv[x] = ld_##SUF(b[x] + l); } \
        for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) s[x][y] += av[x] * bv[y]; \
    }                                                                         \
    for (int x = 0; x < 4; x++) for (int y = 0; y < 4; y++) {                 \
        V v = s[x][y];                                                        \
        T t = 0;                                                              \
        for (int q = 0; q < W; q++) t += v[q];                                \
        for (int64_t q = l; q < n; q++) t += a[x][q] * b[y][q];               \
        out[x * 4 + y] = t;                                                   \
    }                                                                         \
}
DEF_TILE(f64, double, lin_v4d, 4)
DEF_TILE(f32, float,  lin_v8f, 8)

#define DEF_GRAM(SUF, T)                                                      \
static void gram_k_##SUF(const lin_t* A, T* G, int64_t p,                     \
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
                T t[16];                                                      \
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
    void* part;                   /* nt partial p x p results, float or double as A is */
    int64_t p, k, kchunk;
} gram_ctx_t;

static void gram_task(void* vctx, uint32_t wid, int64_t start, int64_t end) {
    (void)wid;
    gram_ctx_t* g = (gram_ctx_t*)vctx;
    for (int64_t t = start; t < end; t++) {
        int64_t k0 = t * g->kchunk, k1 = k0 + g->kchunk;
        if (k1 > g->k) k1 = g->k;
        if (g->A->f32) gram_k_f32(g->A, (float*)g->part + t * g->p * g->p, g->p, k0, k1);
        else           gram_k_f64(g->A, (double*)g->part + t * g->p * g->p, g->p, k0, k1);
    }
}

/* G[p x p] (zeroed here, upper triangle then mirrored) from A (p rows of k):
 * an array of float when A is bound as F32, of double otherwise. */
static void lin_gram(const lin_t* A, void* G, int64_t p, int64_t k) {
    size_t esz = A->f32 ? sizeof(float) : sizeof(double);
    memset(G, 0, (size_t)p * (size_t)p * esz);
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
            g.part = ray_calloc_raw((size_t)nt * (size_t)(p * p) * esz);
            if (!g.part) par = false;
        }
    }
    if (par) {
        ray_pool_dispatch_n(pool, gram_task, &g, (uint32_t)nt);
        for (int64_t t = 0; t < nt; t++) {
            if (A->f32) {
                const float* P = (const float*)g.part + t * p * p;
                float* Gf = (float*)G;
                for (int64_t i = 0; i < p * p; i++) Gf[i] += P[i];
            } else {
                const double* P = (const double*)g.part + t * p * p;
                double* Gd = (double*)G;
                for (int64_t i = 0; i < p * p; i++) Gd[i] += P[i];
            }
        }
        ray_free_raw(g.part);
    } else if (A->f32) gram_k_f32(A, (float*)G, p, 0, k);
    else               gram_k_f64(A, (double*)G, p, 0, k);
    if (A->f32) {
        float* Gf = (float*)G;
        for (int64_t i = 0; i < p; i++)
            for (int64_t j = 0; j < i; j++) Gf[i * p + j] = Gf[j * p + i];
    } else {
        double* Gd = (double*)G;
        for (int64_t i = 0; i < p; i++)
            for (int64_t j = 0; j < i; j++) Gd[i * p + j] = Gd[j * p + i];
    }
}

/* (gram x): x mmu flip x, for x a matrix (list of rows). */
ray_t* ray_gram_fn(ray_t* x) {
    lin_t A;
    ray_t* e;
    bool f32 = lin_all_f32(x);
    if ((e = lin_load("gram", x, &A, f32))) return e;
    if (A.is_vec) { lin_free(&A); return ray_error("type", "gram: expects a matrix (list of numeric rows)"); }
    ray_t* out;
    void* G = ray_calloc_raw((size_t)A.r * (size_t)A.r * (f32 ? sizeof(float) : sizeof(double)));
    if (!G) out = ray_error("oom", NULL);
    else {
        lin_gram(&A, G, A.r, A.c);
        out = f32 ? lin_out_f32((const float*)G, A.r, A.r) : lin_out((const double*)G, A.r, A.r, false);
        ray_free_raw(G);
    }
    lin_free(&A);
    return out;
}

/* In-place Gauss-Jordan with partial pivoting; a is n x n, inv gets A^-1.
 * Returns false when the matrix is singular. */
static bool lin_invert(double* a, double* inv, int64_t n) {
    /* a pivot at or below n*eps times the largest element is rounding noise:
     * the matrix is singular to working precision (same rule as the QR path) */
    double tol = (double)n * DBL_EPSILON * lin_maxabs(a, n * n);
    for (int64_t i = 0; i < n; i++)
        for (int64_t j = 0; j < n; j++) inv[i * n + j] = (i == j);
    for (int64_t k = 0; k < n; k++) {
        int64_t p = k;
        double best = fabs(a[k * n + k]);
        for (int64_t i = k + 1; i < n; i++)
            if (fabs(a[i * n + k]) > best) { best = fabs(a[i * n + k]); p = i; }
        if (!(best > tol)) return false;
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
    if (A.is_vec) {
        out = ray_error("type", "inv: expects a square matrix (list of numeric rows)");
    } else if (A.r != A.c) {
        out = ray_error("length", "inv: expects a square matrix");
    } else if (lin_nonfinite(&A)) {
        out = ray_error("domain", "inv: input contains a null");
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
        if (!(nrm > 0.0)) { ok = false; break; }
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
    double tol = (double)n * DBL_EPSILON * lin_maxabs(M, n * n);   /* as in lin_invert */
    for (int64_t c = 0; c < n; c++) {
        int64_t p = c;
        double best = fabs(M[c * n + c]);
        for (int64_t i = c + 1; i < n; i++)
            if (fabs(M[i * n + c]) > best) { best = fabs(M[i * n + c]); p = i; }
        if (!(best > tol)) return false;
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
    if (lin_nonfinite(&A) || lin_nonfinite(&B)) { out = ray_error("domain", "lsq: input contains a null"); goto done; }
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

/* Transpose of rows that all have type T: the elements are moved as they are,
 * so a null stays the type's own null.  A column holds a null only if its
 * source elements did; the attribute is set from the data. */
static ray_t* lin_flip_same(ray_t* x, int64_t r, int64_t c) {
    uint8_t ty = ray_list_get(x, 0)->type;
    size_t w = ty == RAY_I64 ? 8 : 4;
    ray_t* L = ray_list_new(c);
    if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    for (int64_t j = 0; j < c; j++) {
        ray_t* v = ray_vec_new(ty, r);
        if (!v || RAY_IS_ERR(v)) { ray_release(L); return v ? v : ray_error("oom", NULL); }
        v->len = r;
        char* o = (char*)ray_data(v);
        bool nulls = false;
        for (int64_t i = 0; i < r; i++) {
            ray_t* row = ray_list_get(x, i);
            memcpy(o + (size_t)i * w, (const char*)ray_data(row) + (size_t)j * w, w);
            if (ray_vec_is_null(row, j)) nulls = true;
        }
        if (nulls) v->attrs |= RAY_ATTR_HAS_NULLS;
        L = ray_list_append_take(L, v);
        ray_release(v);
        if (!L || RAY_IS_ERR(L)) return L ? L : ray_error("oom", NULL);
    }
    return L;
}

/* (flip m): transpose of a matrix (list of rows -> list of columns).  A
 * transpose does no arithmetic, so rows of one type keep it; rows of mixed
 * types widen to F64. */
ray_t* ray_flip_mat_fn(ray_t* x) {
    lin_t A;
    ray_t* e;
    if ((e = lin_load("flip", x, &A, false))) return e;
    if (A.is_vec) { lin_free(&A); return ray_error("type", "flip: expects a matrix (list of numeric rows)"); }
    int64_t r = A.r, c = A.c;
    uint8_t ty = ray_list_get(x, 0)->type;
    bool same = true;
    for (int64_t i = 1; i < r; i++) if (ray_list_get(x, i)->type != ty) same = false;
    if (same && ty != RAY_F64) {
        lin_free(&A);
        return lin_flip_same(x, r, c);
    }
    double* T = (double*)ray_calloc_raw((size_t)r * (size_t)c * sizeof(double));
    ray_t* out;
    if (!T) out = ray_error("oom", NULL);
    else {
        for (int64_t i = 0; i < r; i++)
            for (int64_t j = 0; j < c; j++) T[j * r + i] = ((const double*)A.rp[i])[j];
        out = lin_out(T, c, r, false);
        ray_free_raw(T);
    }
    lin_free(&A);
    return out;
}
