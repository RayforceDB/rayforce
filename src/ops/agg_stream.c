/* src/ops/agg_stream.c — streaming accumulators (design §4.5).
 * Inner loops mirror group.c REDUCE_LOOP_I/F but carry ONLY this aggregate's
 * state, recovering the rowform density win generically. */
#include "ops/agg_registry.h"
#include "ops/ops.h"
#include "ops/internal.h"   /* ray_f64_fin (single-null float model) */
#include "lang/internal.h"  /* ray_median_dbl_inplace */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>         /* realloc/free for the buffered median accumulator */
#include <string.h>         /* memcpy for the top_n/bot_n native buffer */

/* Registry dispatch already fixes each native kernel's input representation.
 * Select its sentinel reader at compile time, preserving the validity view's
 * base pointer without repeating a runtime type switch for every row. */
static inline bool agg_live_i16(const void* p, int64_t i) { return ((const int16_t*)p)[i] != NULL_I16; }
static inline bool agg_live_i32(const void* p, int64_t i) { return ((const int32_t*)p)[i] != NULL_I32; }
static inline bool agg_live_i64(const void* p, int64_t i) { return ((const int64_t*)p)[i] != NULL_I64; }
static inline bool agg_live_f32(const void* p, int64_t i) { float v = ((const float*)p)[i]; return v == v; }
static inline bool agg_live_f64(const void* p, int64_t i) { double v = ((const double*)p)[i]; return v == v; }
static inline bool agg_live_u8(const void* p, int64_t i) { (void)p; (void)i; return true; }
#define AGG_NATIVE_LIVE(data, validity, row) _Generic(*(data), \
    int16_t: agg_live_i16, int32_t: agg_live_i32, int64_t: agg_live_i64, \
    float: agg_live_f32, double: agg_live_f64, uint8_t: agg_live_u8)((validity)->base, row)

/* No-null fast path.  has_nulls is loop-invariant, but the compiler does NOT
 * reliably hoist ray_valid_at out of the per-row update — it shows up as ~12% of
 * a sum group-by in profiling.  Branch on it ONCE: the common non-null column
 * runs a tight, check-free, vectorizable loop; nullable columns keep the
 * per-row sentinel check.  BODY is the per-row accumulate, using i/gids[i]. */
#define AGG_UPDATE_LOOP(valid, n, BODY)                            \
    do {                                                           \
        if (!(valid)->has_nulls) {                                 \
            for (int64_t i = 0; i < (n); i++) { BODY; }            \
        } else {                                                   \
            for (int64_t i = 0; i < (n); i++) {                    \
                if (!AGG_NATIVE_LIVE(d, (valid), i)) continue;           \
                BODY;                                              \
            }                                                      \
        }                                                          \
    } while (0)

/* Both output APIs share one primitive result calculation. Streaming group
 * emission writes that payload directly, avoiding one allocation per group. */
#define AGG_SCALAR_FINAL(NAME, TYPE, BOX, IS_NULL) \
static bool NAME##_value(const void* state, void* dst) { \
    TYPE value = NAME##_result(state); \
    memcpy(dst, &value, sizeof(value)); \
    return (IS_NULL); \
} \
static ray_t* NAME(const void* state, acc_arena_t* arena, int64_t param) { \
    (void)arena; (void)param; \
    return BOX(NAME##_result(state)); \
}

/* ---- sum, I64 -------------------------------------------------------- */
typedef struct { int64_t sum; } sum_i64_state;

static void sum_i64_init(void* s) { ((sum_i64_state*)s)->sum = 0; }

static void sum_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    });
}

static void sum_i64_merge(void* dst, const void* src, acc_arena_t* a) {
    (void)a;
    ((sum_i64_state*)dst)->sum = (int64_t)((uint64_t)((sum_i64_state*)dst)->sum
                                         + (uint64_t)((const sum_i64_state*)src)->sum);
}

static int64_t sum_i64_final_result(const void* s) {
    return ((const sum_i64_state*)s)->sum;
}
AGG_SCALAR_FINAL(sum_i64_final, int64_t, ray_i64, value == NULL_I64)

static const agg_vtable_t SUM_I64 = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_i64_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final, .finalize_value = sum_i64_final_value,
};

/* ---- count (type-agnostic over live rows) ---------------------------- */
typedef struct { int64_t n; } count_state;
static void count_init(void* s) { ((count_state*)s)->n = 0; }
static void count_update(void* base, size_t stride, const uint32_t* gids,
                         const void* vals, const ray_valid_t* valid,
                         int64_t n, acc_arena_t* a) {
    (void)vals; (void)a; (void)valid;
    for (int64_t i = 0; i < n; i++)
        ((count_state*)((char*)base + (size_t)gids[i]*stride))->n++;
}
static void count_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((count_state*)d)->n += ((const count_state*)s)->n;
}
static int64_t count_final_result(const void* s) {
    return ((const count_state*)s)->n;
}
AGG_SCALAR_FINAL(count_final, int64_t, ray_i64, value == NULL_I64)
static const agg_vtable_t COUNT_ANY = {
    .state_size = sizeof(count_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = count_init, .update_batch = count_update,
    .merge = count_merge, .finalize = count_final, .finalize_value = count_final_value,
};

/* ---- min / max I64 (empty group → typed null) ------------------------ */
typedef struct { int64_t v; int64_t cnt; } ext_i64_state;
/* Narrow native extrema need a value and validity, not a 64-bit count. */
typedef struct { _Alignas(8) int32_t v; uint32_t seen; } ext_i32_state;
_Static_assert(sizeof(ext_i32_state) == 8, "shared extrema require one aligned 64-bit state");
static void min_i32_init(void* s) { *(ext_i32_state*)s = (ext_i32_state){ INT32_MAX, 0 }; }
static void max_i32_init(void* s) { *(ext_i32_state*)s = (ext_i32_state){ INT32_MIN, 0 }; }
static void min_i32_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i32_state* src = s; ext_i32_state* dst = d;
    if (src->seen && src->v < dst->v) dst->v = src->v;
    dst->seen |= src->seen;
}
static void max_i32_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i32_state* src = s; ext_i32_state* dst = d;
    if (src->seen && src->v > dst->v) dst->v = src->v;
    dst->seen |= src->seen;
}
/* Extrema only write when a value improves the result. Repeated hot keys
 * therefore become shared reads once their current bound is established. */
#if ATOMIC_LLONG_LOCK_FREE == 2
#define NARROW_SHARED(NAME, T, CMP) \
static void NAME(void* base, size_t stride, const uint32_t* gids, \
                 const void* vals, const ray_valid_t* valid, int64_t n) { \
    const T* d = vals; \
    AGG_UPDATE_LOOP(valid, n, { \
        ext_i32_state* state = (ext_i32_state*)((char*)base + (size_t)gids[i] * stride); \
        ext_i32_state old; \
        __atomic_load(state, &old, __ATOMIC_RELAXED); \
        while (!old.seen || d[i] CMP old.v) { \
            ext_i32_state next; next.v = d[i]; next.seen = 1; \
            if (__atomic_compare_exchange(state, &old, &next, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break; \
        } \
    }); \
}
NARROW_SHARED(min_bool_shared, uint8_t, <)
NARROW_SHARED(max_bool_shared, uint8_t, >)
NARROW_SHARED(min_u8_shared, uint8_t, <)
NARROW_SHARED(max_u8_shared, uint8_t, >)
NARROW_SHARED(min_i16_shared, int16_t, <)
NARROW_SHARED(max_i16_shared, int16_t, >)
NARROW_SHARED(min_i32_shared, int32_t, <)
NARROW_SHARED(max_i32_shared, int32_t, >)
NARROW_SHARED(min_date_shared, int32_t, <)
NARROW_SHARED(max_date_shared, int32_t, >)
NARROW_SHARED(min_time_shared, int32_t, <)
NARROW_SHARED(max_time_shared, int32_t, >)
#undef NARROW_SHARED
#define SHARED_UPDATE(NAME) .update_shared = NAME,
#else
#define SHARED_UPDATE(NAME)
#endif

static void min_i64_init(void* s) { ((ext_i64_state*)s)->v = INT64_MAX; ((ext_i64_state*)s)->cnt = 0; }
static void max_i64_init(void* s) { ((ext_i64_state*)s)->v = INT64_MIN; ((ext_i64_state*)s)->cnt = 0; }
static void min_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    });
}
static void max_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    });
}
static void min_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* src = s; ext_i64_state* dst = d;
    if (src->cnt && src->v < dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static void max_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_i64_state* src = s; ext_i64_state* dst = d;
    if (src->cnt && src->v > dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static int64_t ext_i64_final_result(const void* s) {
    const ext_i64_state* st = s;
    return st->cnt ? st->v : NULL_I64;
}
AGG_SCALAR_FINAL(ext_i64_final, int64_t, ray_i64, value == NULL_I64)
static const agg_vtable_t MIN_I64 = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = min_i64_init, .update_batch = min_i64_update,
    .merge = min_i64_merge, .finalize = ext_i64_final, .finalize_value = ext_i64_final_value,
};
static const agg_vtable_t MAX_I64 = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = max_i64_init, .update_batch = max_i64_update,
    .merge = max_i64_merge, .finalize = ext_i64_final, .finalize_value = ext_i64_final_value,
};

/* ---- sum, F64 -------------------------------------------------------- */
typedef struct { double sum; } sum_f64_state;
static void sum_f64_init(void* s) { ((sum_f64_state*)s)->sum = 0.0; }
static void sum_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    AGG_UPDATE_LOOP(valid, n,
        ((sum_f64_state*)((char*)base + (size_t)gids[i]*stride))->sum += d[i]);
}
static void sum_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((sum_f64_state*)d)->sum += ((const sum_f64_state*)s)->sum;
}
static double sum_f64_final_result(const void* s) {
    return ray_f64_fin(((const sum_f64_state*)s)->sum);
}
AGG_SCALAR_FINAL(sum_f64_final, double, ray_f64, value != value)
static const agg_vtable_t SUM_F64 = {
    .state_size = sizeof(sum_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = sum_f64_init, .update_batch = sum_f64_update,
    .merge = sum_f64_merge, .finalize = sum_f64_final, .finalize_value = sum_f64_final_value,
};

/* ---- min / max F64 (empty group → typed null) ------------------------ */
typedef struct { double v; int64_t cnt; } ext_f64_state;
static void min_f64_init(void* s) { ((ext_f64_state*)s)->v = INFINITY;  ((ext_f64_state*)s)->cnt = 0; }
static void max_f64_init(void* s) { ((ext_f64_state*)s)->v = -INFINITY; ((ext_f64_state*)s)->cnt = 0; }
static void min_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_f64_state* st = (ext_f64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    });
}
static void max_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_f64_state* st = (ext_f64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    });
}
static void min_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_f64_state* src = s; ext_f64_state* dst = d;
    if (src->cnt && src->v < dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static void max_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; const ext_f64_state* src = s; ext_f64_state* dst = d;
    if (src->cnt && src->v > dst->v) { dst->v = src->v; }
    dst->cnt += src->cnt;
}
static double ext_f64_final_result(const void* s) {
    const ext_f64_state* st = s;
    /* min/max of finite inputs is finite, but ray_f64_fin guards against an
     * ±Inf that may have leaked in from a not-yet-canonicalized source. */
    return st->cnt ? ray_f64_fin(st->v) : NULL_F64;
}
AGG_SCALAR_FINAL(ext_f64_final, double, ray_f64, value != value)
static const agg_vtable_t MIN_F64 = {
    .state_size = sizeof(ext_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = min_f64_init, .update_batch = min_f64_update,
    .merge = min_f64_merge, .finalize = ext_f64_final, .finalize_value = ext_f64_final_value,
};
static const agg_vtable_t MAX_F64 = {
    .state_size = sizeof(ext_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = max_f64_init, .update_batch = max_f64_update,
    .merge = max_f64_merge, .finalize = ext_f64_final, .finalize_value = ext_f64_final_value,
};

/* ---- avg, F64 -------------------------------------------------------- */
typedef struct { double sum; int64_t cnt; } avg_f64_state;
static void avg_f64_init(void* s) { ((avg_f64_state*)s)->sum = 0.0; ((avg_f64_state*)s)->cnt = 0; }
static void avg_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}
static void avg_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; ((avg_f64_state*)d)->sum += ((const avg_f64_state*)s)->sum;
    ((avg_f64_state*)d)->cnt += ((const avg_f64_state*)s)->cnt;
}
static double avg_f64_final_result(const void* s) {
    const avg_f64_state* st = s;
    return st->cnt ? ray_f64_fin(st->sum / (double)st->cnt) : NULL_F64;
}
AGG_SCALAR_FINAL(avg_f64_final, double, ray_f64, value != value)
static const agg_vtable_t AVG_F64 = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_f64_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

/* ---- variance family, I64 (sumsq as int64 unsigned-wrap; formula group.c:2190) -- */
typedef struct { double sum; int64_t sumsq; int64_t cnt; } var_i64_state;
static void var_i64_init(void* s) { var_i64_state* st = s; st->sum = 0; st->sumsq = 0; st->cnt = 0; }
static void var_i64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}
static void var_i64_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; var_i64_state* d = dd; const var_i64_state* s = ss;
    d->sum += s->sum; d->sumsq = (int64_t)((uint64_t)d->sumsq + (uint64_t)s->sumsq); d->cnt += s->cnt;
}
static inline double var_i64_varpop(const var_i64_state* st) {
    double mean = st->sum / (double)st->cnt;
    double vp = (double)st->sumsq / (double)st->cnt - mean*mean;
    return vp < 0 ? 0 : vp;
}
static double fin_var_pop_i64_result(const void* s) {
    const var_i64_state* st = s;
    if (st->cnt <= 0) return NULL_F64;
    return ray_f64_fin(var_i64_varpop(st));
}
AGG_SCALAR_FINAL(fin_var_pop_i64, double, ray_f64, value != value)
static double fin_var_i64_result(const void* s) {
    const var_i64_state* st = s;
    if (st->cnt <= 1) return NULL_F64;
    return ray_f64_fin(var_i64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0));
}
AGG_SCALAR_FINAL(fin_var_i64, double, ray_f64, value != value)
static double fin_stddev_pop_i64_result(const void* s) {
    const var_i64_state* st = s;
    if (st->cnt <= 0) return NULL_F64;
    return ray_f64_fin(sqrt(var_i64_varpop(st)));
}
AGG_SCALAR_FINAL(fin_stddev_pop_i64, double, ray_f64, value != value)
static double fin_stddev_i64_result(const void* s) {
    const var_i64_state* st = s;
    if (st->cnt <= 1) return NULL_F64;
    return ray_f64_fin(sqrt(var_i64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0)));
}
AGG_SCALAR_FINAL(fin_stddev_i64, double, ray_f64, value != value)
static const agg_vtable_t VAR_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};
static const agg_vtable_t VAR_POP_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};
static const agg_vtable_t STDDEV_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};
static const agg_vtable_t STDDEV_POP_I64 = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i64_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

/* ---- variance family, F64 (sumsq as double) -------------------------- */
typedef struct { double sum; double sumsq; int64_t cnt; } var_f64_state;
static void var_f64_init(void* s) { var_f64_state* st = s; st->sum = 0; st->sumsq = 0; st->cnt = 0; }
static void var_f64_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const double* d = (const double*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_f64_state* st = (var_f64_state*)((char*)base + (size_t)gids[i]*stride);
        double v = d[i]; st->sum += v; st->sumsq += v*v; st->cnt++;
    });
}
static void var_f64_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; var_f64_state* d = dd; const var_f64_state* s = ss;
    d->sum += s->sum; d->sumsq += s->sumsq; d->cnt += s->cnt;
}
static inline double var_f64_varpop(const var_f64_state* st) {
    double mean = st->sum / (double)st->cnt;
    double vp = st->sumsq / (double)st->cnt - mean*mean;
    return vp < 0 ? 0 : vp;
}
static double fin_var_pop_f64_result(const void* s) {
    const var_f64_state* st = s;
    if (st->cnt <= 0) return NULL_F64;
    return ray_f64_fin(var_f64_varpop(st));
}
AGG_SCALAR_FINAL(fin_var_pop_f64, double, ray_f64, value != value)
static double fin_var_f64_result(const void* s) {
    const var_f64_state* st = s;
    if (st->cnt <= 1) return NULL_F64;
    return ray_f64_fin(var_f64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0));
}
AGG_SCALAR_FINAL(fin_var_f64, double, ray_f64, value != value)
static double fin_stddev_pop_f64_result(const void* s) {
    const var_f64_state* st = s;
    if (st->cnt <= 0) return NULL_F64;
    return ray_f64_fin(sqrt(var_f64_varpop(st)));
}
AGG_SCALAR_FINAL(fin_stddev_pop_f64, double, ray_f64, value != value)
static double fin_stddev_f64_result(const void* s) {
    const var_f64_state* st = s;
    if (st->cnt <= 1) return NULL_F64;
    return ray_f64_fin(sqrt(var_f64_varpop(st) * (double)st->cnt / ((double)st->cnt - 1.0)));
}
AGG_SCALAR_FINAL(fin_stddev_f64, double, ray_f64, value != value)
static const agg_vtable_t VAR_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_var_f64, .finalize_value = fin_var_f64_value,
};
static const agg_vtable_t VAR_POP_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_var_pop_f64, .finalize_value = fin_var_pop_f64_value,
};
static const agg_vtable_t STDDEV_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_stddev_f64, .finalize_value = fin_stddev_f64_value,
};
static const agg_vtable_t STDDEV_POP_F64 = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f64_update,
    .merge = var_f64_merge, .finalize = fin_stddev_pop_f64, .finalize_value = fin_stddev_pop_f64_value,
};

/* ---- pairwise numeric aggregates, F64 output (binary input: x,y) ------- */
typedef struct { double sx, sy, sxx, syy, sxy; int64_t n; } pearson_state;
static void pearson_init(void* s) {
    pearson_state* st = s; st->sx = st->sy = st->sxx = st->syy = st->sxy = 0; st->n = 0;
}
/* Read element `i` of a numeric/temporal column (per the valid view's type)
 * widened to double.  Lets pearson accept integer inputs without forcing the
 * caller to materialize an F64 copy. */
static inline double pearson_read_f64(const ray_valid_t* v, int64_t i) {
    switch (v->type) {
        case RAY_F32:                       return ((const float*)v->base)[i];
        case RAY_F64:                       return ((const double*)v->base)[i];
        case RAY_I64: case RAY_TIMESTAMP:   return (double)((const int64_t*)v->base)[i];
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                                            return (double)((const int32_t*)v->base)[i];
        case RAY_I16:                       return (double)((const int16_t*)v->base)[i];
        case RAY_U8:  case RAY_BOOL:        return (double)((const uint8_t*)v->base)[i];
        default:                            return 0.0;  /* gate admits only the above */
    }
}
static void pearson_update2(void* base, size_t stride, const uint32_t* gids,
                            const void* vx, const void* vy,
                            const ray_valid_t* valx, const ray_valid_t* valy,
                            int64_t n, acc_arena_t* a) {
    (void)a; (void)vx; (void)vy;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valx, i) || !ray_valid_at(valy, i)) continue;
        pearson_state* st = (pearson_state*)((char*)base + (size_t)gids[i]*stride);
        double xi = pearson_read_f64(valx, i), yi = pearson_read_f64(valy, i);
        st->sx += xi; st->sy += yi; st->sxx += xi*xi; st->syy += yi*yi; st->sxy += xi*yi; st->n++;
    }
}
static void pearson_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; pearson_state* d = dd; const pearson_state* s = ss;
    d->sx += s->sx; d->sy += s->sy; d->sxx += s->sxx; d->syy += s->syy; d->sxy += s->sxy; d->n += s->n;
}
static double pearson_final_result(const void* s) {
    const pearson_state* st = s;
    double dn = (double)st->n;
    double num = dn*st->sxy - st->sx*st->sy,
           dx  = dn*st->sxx - st->sx*st->sx,
           dy  = dn*st->syy - st->sy*st->sy;
    /* dx and dy are >= 0 mathematically (Cauchy-Schwarz), but a CONSTANT
     * column cancels to a small NEGATIVE residue in doubles.  Relying on
     * sqrt(<=0) to produce NaN only works when exactly ONE side is
     * negative: with both negative the product is positive, the root is
     * finite and a garbage "correlation" outside [-1,1] is emitted
     * (#555).  Test the two denominators separately, as the legacy keyed
     * path does (group.c), and report the undefined case as null.
     *
     * n < 2 leaves dx = dy = 0, so the same guard covers it. */
    if (dx <= 0.0 || dy <= 0.0) return NULL_F64;
    return ray_f64_fin(num / sqrt(dx*dy));
}
AGG_SCALAR_FINAL(pearson_final, double, ray_f64, value != value)
static double cov_final_result(const void* s) {
    const pearson_state* st = s;
    if (st->n <= 0) return NULL_F64;
    double dn = (double)st->n;
    double v = (st->sxy - (st->sx * st->sy) / dn) / dn;
    return ray_f64_fin(v);
}
AGG_SCALAR_FINAL(cov_final, double, ray_f64, value != value)
static double scov_final_result(const void* s) {
    const pearson_state* st = s;
    if (st->n <= 1) return NULL_F64;
    double dn = (double)st->n;
    double v = (st->sxy - (st->sx * st->sy) / dn) / (dn - 1.0);
    return ray_f64_fin(v);
}
AGG_SCALAR_FINAL(scov_final, double, ray_f64, value != value)
static double wsum_final_result(const void* s) {
    const pearson_state* st = s;
    return ray_f64_fin(st->sxy);
}
AGG_SCALAR_FINAL(wsum_final, double, ray_f64, value != value)
static double wavg_final_result(const void* s) {
    const pearson_state* st = s;
    if (st->n <= 0 || st->sx == 0.0) return NULL_F64;
    return ray_f64_fin(st->sxy / st->sx);
}
AGG_SCALAR_FINAL(wavg_final, double, ray_f64, value != value)
static const agg_vtable_t PEARSON_F64 = {
    .state_size = sizeof(pearson_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = pearson_init, .update_batch2 = pearson_update2,
    .merge = pearson_merge, .finalize = pearson_final, .finalize_value = pearson_final_value,
};
static const agg_vtable_t COV_F64 = {
    .state_size = sizeof(pearson_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = pearson_init, .update_batch2 = pearson_update2,
    .merge = pearson_merge, .finalize = cov_final, .finalize_value = cov_final_value,
};
static const agg_vtable_t SCOV_F64 = {
    .state_size = sizeof(pearson_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = pearson_init, .update_batch2 = pearson_update2,
    .merge = pearson_merge, .finalize = scov_final, .finalize_value = scov_final_value,
};
static const agg_vtable_t WSUM_F64 = {
    .state_size = sizeof(pearson_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = pearson_init, .update_batch2 = pearson_update2,
    .merge = pearson_merge, .finalize = wsum_final, .finalize_value = wsum_final_value,
};
static const agg_vtable_t WAVG_F64 = {
    .state_size = sizeof(pearson_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = pearson_init, .update_batch2 = pearson_update2,
    .merge = pearson_merge, .finalize = wavg_final, .finalize_value = wavg_final_value,
};

/* ---- all / any truth reductions, BOOL output -------------------------- */
typedef struct { int64_t n; int64_t truthy; } truth_state;
static void truth_init(void* s) { truth_state* st = s; st->n = 0; st->truthy = 0; }
static inline int truth_read(const ray_valid_t* v, int64_t i) {
    switch (v->type) {
        case RAY_F32:                       return ((const float*)v->base)[i] != 0.0f;
        case RAY_F64:                       return ((const double*)v->base)[i] != 0.0;
        case RAY_I64: case RAY_TIMESTAMP:   return ((const int64_t*)v->base)[i] != 0;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
                                            return ((const int32_t*)v->base)[i] != 0;
        case RAY_I16:                       return ((const int16_t*)v->base)[i] != 0;
        case RAY_U8:  case RAY_BOOL:        return ((const uint8_t*)v->base)[i] != 0;
        default:                            return 0;
    }
}
static void truth_update(void* base, size_t stride, const uint32_t* gids,
                         const void* vals, const ray_valid_t* valid,
                         int64_t n, acc_arena_t* arena) {
    (void)arena; (void)vals;
    for (int64_t i = 0; i < n; i++) {
        if (!ray_valid_at(valid, i)) continue;
        truth_state* st = (truth_state*)((char*)base + (size_t)gids[i]*stride);
        st->n++;
        st->truthy += truth_read(valid, i) ? 1 : 0;
    }
}
static void truth_merge(void* dd, const void* ss, acc_arena_t* a) {
    (void)a; truth_state* d = dd; const truth_state* s = ss;
    d->n += s->n; d->truthy += s->truthy;
}
static uint8_t all_final_result(const void* s) {
    const truth_state* st = s;
    return st->truthy == st->n;
}
AGG_SCALAR_FINAL(all_final, uint8_t, ray_bool, false)
static uint8_t any_final_result(const void* s) {
    const truth_state* st = s;
    return st->truthy > 0;
}
AGG_SCALAR_FINAL(any_final, uint8_t, ray_bool, false)
static const agg_vtable_t ALL_BOOL = {
    .state_size = sizeof(truth_state), .kind = ACC_STREAMING, .out_type = RAY_BOOL,
    .init = truth_init, .update_batch = truth_update,
    .merge = truth_merge, .finalize = all_final, .finalize_value = all_final_value,
};
static const agg_vtable_t ANY_BOOL = {
    .state_size = sizeof(truth_state), .kind = ACC_STREAMING, .out_type = RAY_BOOL,
    .init = truth_init, .update_batch = truth_update,
    .merge = truth_merge, .finalize = any_final, .finalize_value = any_final_value,
};

/* ---- median, F64 output (first ACC_BUFFERED: growable per-group buffer) ---- */
typedef struct { double* buf; int64_t len; int64_t cap; } median_state;
static void median_init(void* s){ median_state* st=s; st->buf=NULL; st->len=0; st->cap=0; }
static inline void median_push(median_state* st, double v){
    if (st->len == st->cap){ int64_t nc = st->cap ? st->cap*2 : 8;
        double* nb = ray_realloc_raw(st->buf, (size_t)nc*sizeof(double)); st->buf=nb; st->cap=nc; }
    st->buf[st->len++] = v;
}
static void median_update_i64(void* base, size_t stride, const uint32_t* gids,
                              const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const int64_t* d=vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        median_push((median_state*)((char*)base+(size_t)gids[i]*stride),(double)d[i]); }
}
static void median_update_f64(void* base, size_t stride, const uint32_t* gids,
                              const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a){
    (void)a; const double* d=vals;
    for (int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        median_push((median_state*)((char*)base+(size_t)gids[i]*stride), d[i]); }
}
static void median_merge(void* dd, const void* ss, acc_arena_t* a){ (void)a;
    median_state* d=dd; const median_state* s=ss;
    for (int64_t i=0;i<s->len;i++) median_push(d, s->buf[i]); }
static ray_t* median_final(const void* s, acc_arena_t* a, int64_t param){ (void)a; (void)param; median_state* st=(median_state*)s;
    if (st->len==0) return ray_typed_null(-RAY_F64);
    return ray_f64(ray_median_dbl_inplace(st->buf, st->len)); }
static void median_destroy(void* s){ median_state* st=s; ray_free_raw(st->buf); st->buf=NULL; st->len=st->cap=0; }
static const agg_vtable_t MEDIAN_I64 = { .state_size=sizeof(median_state), .kind=ACC_BUFFERED, .out_type=RAY_F64,
    .init=median_init, .update_batch=median_update_i64, .merge=median_merge, .finalize=median_final, .destroy=median_destroy };
static const agg_vtable_t MEDIAN_F64 = { .state_size=sizeof(median_state), .kind=ACC_BUFFERED, .out_type=RAY_F64,
    .init=median_init, .update_batch=median_update_f64, .merge=median_merge, .finalize=median_final, .destroy=median_destroy };

/* ---- top_n / bot_n: ACC_BUFFERED, NATIVE-typed buffer, LIST cell output ----
 * finalize wraps the per-group buffer as a vec and reuses topk_take_vec for
 * value+order parity with the old engine (top=desc1 largest-first, bot=desc0
 * smallest-first).  out_type is RAY_LIST: the result cell is a vector of the
 * native input type (min(K,len) elements, ordered). */
typedef struct { int64_t* buf; int64_t len; int64_t cap; } topk_i64_state;
static void topk_i64_init(void* s){ topk_i64_state* st=s; st->buf=NULL; st->len=0; st->cap=0; }
static inline void topk_i64_push(topk_i64_state* st, int64_t v){
    if (st->len==st->cap){ int64_t nc=st->cap?st->cap*2:8;
        int64_t* nb=ray_realloc_raw(st->buf,(size_t)nc*sizeof(int64_t)); st->buf=nb; st->cap=nc; }
    st->buf[st->len++]=v; }
static void topk_i64_update(void* base,size_t stride,const uint32_t* gids,const void* vals,
                            const ray_valid_t* valid,int64_t n,acc_arena_t* a){ (void)a;
    const int64_t* d=vals;
    for(int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        topk_i64_push((topk_i64_state*)((char*)base+(size_t)gids[i]*stride),d[i]); } }
static void topk_i64_merge(void* dd,const void* ss,acc_arena_t* a){ (void)a;
    topk_i64_state* d=dd; const topk_i64_state* s=ss;
    for(int64_t i=0;i<s->len;i++) topk_i64_push(d,s->buf[i]); }
static void topk_i64_destroy(void* s){ topk_i64_state* st=s; ray_free_raw(st->buf); st->buf=NULL; st->len=st->cap=0; }
static ray_t* topk_i64_make(const topk_i64_state* st, int64_t k, uint8_t desc){
    if (st->len==0) return ray_vec_new(RAY_I64, 0);  /* empty group → 0-len vec */
    ray_t* v=ray_vec_new(RAY_I64, st->len); v->len=st->len;
    memcpy(ray_data(v), st->buf, (size_t)st->len*sizeof(int64_t));
    ray_t* out=topk_take_vec(v, k, desc); ray_release(v);
    /* k>=len takes the asc/desc fast path → a LAZY handle; the LIST cell must
     * be a concrete vector, so materialize before handing it back. */
    if (out && !RAY_IS_ERR(out) && ray_is_lazy(out)) out=ray_lazy_materialize(out);
    return out; }
static ray_t* topN_i64_final(const void* s,acc_arena_t* a,int64_t k){ (void)a; return topk_i64_make(s,k,1); }
static ray_t* botN_i64_final(const void* s,acc_arena_t* a,int64_t k){ (void)a; return topk_i64_make(s,k,0); }
static const agg_vtable_t TOPK_I64 = { .state_size=sizeof(topk_i64_state), .kind=ACC_BUFFERED, .out_type=RAY_LIST,
    .init=topk_i64_init, .update_batch=topk_i64_update, .merge=topk_i64_merge, .finalize=topN_i64_final, .destroy=topk_i64_destroy };
static const agg_vtable_t BOTK_I64 = { .state_size=sizeof(topk_i64_state), .kind=ACC_BUFFERED, .out_type=RAY_LIST,
    .init=topk_i64_init, .update_batch=topk_i64_update, .merge=topk_i64_merge, .finalize=botN_i64_final, .destroy=topk_i64_destroy };

typedef struct { double* buf; int64_t len; int64_t cap; } topk_f64_state;
static void topk_f64_init(void* s){ topk_f64_state* st=s; st->buf=NULL; st->len=0; st->cap=0; }
static inline void topk_f64_push(topk_f64_state* st, double v){
    if (st->len==st->cap){ int64_t nc=st->cap?st->cap*2:8;
        double* nb=ray_realloc_raw(st->buf,(size_t)nc*sizeof(double)); st->buf=nb; st->cap=nc; }
    st->buf[st->len++]=v; }
static void topk_f64_update(void* base,size_t stride,const uint32_t* gids,const void* vals,
                            const ray_valid_t* valid,int64_t n,acc_arena_t* a){ (void)a;
    const double* d=vals;
    for(int64_t i=0;i<n;i++){ if(!ray_valid_at(valid,i))continue;
        topk_f64_push((topk_f64_state*)((char*)base+(size_t)gids[i]*stride),d[i]); } }
static void topk_f64_merge(void* dd,const void* ss,acc_arena_t* a){ (void)a;
    topk_f64_state* d=dd; const topk_f64_state* s=ss;
    for(int64_t i=0;i<s->len;i++) topk_f64_push(d,s->buf[i]); }
static void topk_f64_destroy(void* s){ topk_f64_state* st=s; ray_free_raw(st->buf); st->buf=NULL; st->len=st->cap=0; }
static ray_t* topk_f64_make(const topk_f64_state* st, int64_t k, uint8_t desc){
    if (st->len==0) return ray_vec_new(RAY_F64, 0);
    ray_t* v=ray_vec_new(RAY_F64, st->len); v->len=st->len;
    memcpy(ray_data(v), st->buf, (size_t)st->len*sizeof(double));
    ray_t* out=topk_take_vec(v, k, desc); ray_release(v);
    if (out && !RAY_IS_ERR(out) && ray_is_lazy(out)) out=ray_lazy_materialize(out);
    return out; }
static ray_t* topN_f64_final(const void* s,acc_arena_t* a,int64_t k){ (void)a; return topk_f64_make(s,k,1); }
static ray_t* botN_f64_final(const void* s,acc_arena_t* a,int64_t k){ (void)a; return topk_f64_make(s,k,0); }
static const agg_vtable_t TOPK_F64 = { .state_size=sizeof(topk_f64_state), .kind=ACC_BUFFERED, .out_type=RAY_LIST,
    .init=topk_f64_init, .update_batch=topk_f64_update, .merge=topk_f64_merge, .finalize=topN_f64_final, .destroy=topk_f64_destroy };
static const agg_vtable_t BOTK_F64 = { .state_size=sizeof(topk_f64_state), .kind=ACC_BUFFERED, .out_type=RAY_LIST,
    .init=topk_f64_init, .update_batch=topk_f64_update, .merge=topk_f64_merge, .finalize=botN_f64_final, .destroy=topk_f64_destroy };

/* Native-width streaming readers share the established accumulator states. */

static void min_bool_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->seen = 1;
    });
}

static bool ext_bool_native_value(const void* s, void* dst) {
    const ext_i32_state* st = s;
    *(uint8_t*)dst = st->seen ? (uint8_t)st->v : 0;
    return !st->seen;
}
static ray_t* ext_bool_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i32_state* st = s;
    return st->seen ? ray_bool(st->v) : ray_typed_null(-RAY_BOOL);
}

static const agg_vtable_t MIN_BOOL_NATIVE = {
    SHARED_UPDATE(min_bool_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_BOOL,
    .init = min_i32_init, .update_batch = min_bool_native_update,
    .merge = min_i32_merge, .finalize = ext_bool_native_final, .finalize_value = ext_bool_native_value,
};

static void max_bool_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->seen = 1;
    });
}

static const agg_vtable_t MAX_BOOL_NATIVE = {
    SHARED_UPDATE(max_bool_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_BOOL,
    .init = max_i32_init, .update_batch = max_bool_native_update,
    .merge = max_i32_merge, .finalize = ext_bool_native_final, .finalize_value = ext_bool_native_value,
};

static void avg_bool_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_BOOL_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_bool_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_bool_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_BOOL_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_bool_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_BOOL_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_bool_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_BOOL_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_bool_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_BOOL_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_bool_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

static void sum_bool_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    });
}

static const agg_vtable_t SUM_BOOL_NATIVE = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_bool_native_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final, .finalize_value = sum_i64_final_value,
};

static void min_u8_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->seen = 1;
    });
}

static bool ext_u8_native_value(const void* s, void* dst) {
    const ext_i32_state* st = s;
    *(uint8_t*)dst = st->seen ? (uint8_t)st->v : 0;
    return !st->seen;
}
static ray_t* ext_u8_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i32_state* st = s;
    return st->seen ? ray_u8(st->v) : ray_typed_null(-RAY_U8);
}

static const agg_vtable_t MIN_U8_NATIVE = {
    SHARED_UPDATE(min_u8_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_U8,
    .init = min_i32_init, .update_batch = min_u8_native_update,
    .merge = min_i32_merge, .finalize = ext_u8_native_final, .finalize_value = ext_u8_native_value,
};

static void max_u8_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->seen = 1;
    });
}

static const agg_vtable_t MAX_U8_NATIVE = {
    SHARED_UPDATE(max_u8_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_U8,
    .init = max_i32_init, .update_batch = max_u8_native_update,
    .merge = max_i32_merge, .finalize = ext_u8_native_final, .finalize_value = ext_u8_native_value,
};

static void avg_u8_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_U8_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_u8_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_u8_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_U8_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_u8_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_U8_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_u8_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_U8_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_u8_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_U8_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_u8_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

static void sum_u8_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const uint8_t* d = (const uint8_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    });
}

static const agg_vtable_t SUM_U8_NATIVE = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_u8_native_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final, .finalize_value = sum_i64_final_value,
};

static void min_i16_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int16_t* d = (const int16_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->seen = 1;
    });
}

static bool ext_i16_native_value(const void* s, void* dst) {
    const ext_i32_state* st = s;
    *(int16_t*)dst = st->seen ? (int16_t)st->v : NULL_I16;
    return !st->seen;
}
static ray_t* ext_i16_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i32_state* st = s;
    return st->seen ? ray_i16(st->v) : ray_typed_null(-RAY_I16);
}

static const agg_vtable_t MIN_I16_NATIVE = {
    SHARED_UPDATE(min_i16_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_I16,
    .init = min_i32_init, .update_batch = min_i16_native_update,
    .merge = min_i32_merge, .finalize = ext_i16_native_final, .finalize_value = ext_i16_native_value,
};

static void max_i16_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int16_t* d = (const int16_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->seen = 1;
    });
}

static const agg_vtable_t MAX_I16_NATIVE = {
    SHARED_UPDATE(max_i16_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_I16,
    .init = max_i32_init, .update_batch = max_i16_native_update,
    .merge = max_i32_merge, .finalize = ext_i16_native_final, .finalize_value = ext_i16_native_value,
};

static void avg_i16_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int16_t* d = (const int16_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_I16_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_i16_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_i16_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int16_t* d = (const int16_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_I16_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i16_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_I16_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i16_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_I16_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i16_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_I16_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i16_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

static void sum_i16_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const int16_t* d = (const int16_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    });
}

static const agg_vtable_t SUM_I16_NATIVE = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_i16_native_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final, .finalize_value = sum_i64_final_value,
};

static void min_i32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->seen = 1;
    });
}

static bool ext_i32_native_value(const void* s, void* dst) {
    const ext_i32_state* st = s;
    *(int32_t*)dst = st->seen ? (int32_t)st->v : NULL_I32;
    return !st->seen;
}
static ray_t* ext_i32_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i32_state* st = s;
    return st->seen ? ray_i32(st->v) : ray_typed_null(-RAY_I32);
}

static const agg_vtable_t MIN_I32_NATIVE = {
    SHARED_UPDATE(min_i32_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_I32,
    .init = min_i32_init, .update_batch = min_i32_native_update,
    .merge = min_i32_merge, .finalize = ext_i32_native_final, .finalize_value = ext_i32_native_value,
};

static void max_i32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->seen = 1;
    });
}

static const agg_vtable_t MAX_I32_NATIVE = {
    SHARED_UPDATE(max_i32_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_I32,
    .init = max_i32_init, .update_batch = max_i32_native_update,
    .merge = max_i32_merge, .finalize = ext_i32_native_final, .finalize_value = ext_i32_native_value,
};

static void avg_i32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_I32_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_i32_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_i32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_I32_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i32_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_I32_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i32_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_I32_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i32_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_I32_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_i32_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

static void sum_i32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    });
}

static const agg_vtable_t SUM_I32_NATIVE = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = sum_i64_init, .update_batch = sum_i32_native_update,
    .merge = sum_i64_merge, .finalize = sum_i64_final, .finalize_value = sum_i64_final_value,
};

static void avg_i64_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_I64_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_i64_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void min_f32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const float* d = (const float*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_f64_state* st = (ext_f64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    });
}

static const agg_vtable_t MIN_F32_NATIVE = {
    .state_size = sizeof(ext_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = min_f64_init, .update_batch = min_f32_native_update,
    .merge = min_f64_merge, .finalize = ext_f64_final, .finalize_value = ext_f64_final_value,
};

static void max_f32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const float* d = (const float*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_f64_state* st = (ext_f64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    });
}

static const agg_vtable_t MAX_F32_NATIVE = {
    .state_size = sizeof(ext_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = max_f64_init, .update_batch = max_f32_native_update,
    .merge = max_f64_merge, .finalize = ext_f64_final, .finalize_value = ext_f64_final_value,
};

static void avg_f32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const float* d = (const float*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_F32_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_f32_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_f32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const float* d = (const float*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_f64_state* st = (var_f64_state*)((char*)base + (size_t)gids[i]*stride);
        double v = d[i]; st->sum += v; st->sumsq += v*v; st->cnt++;
    });
}

static const agg_vtable_t VAR_F32_NATIVE = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f32_native_update,
    .merge = var_f64_merge, .finalize = fin_var_f64, .finalize_value = fin_var_f64_value,
};

static const agg_vtable_t VAR_POP_F32_NATIVE = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f32_native_update,
    .merge = var_f64_merge, .finalize = fin_var_pop_f64, .finalize_value = fin_var_pop_f64_value,
};

static const agg_vtable_t STDDEV_F32_NATIVE = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f32_native_update,
    .merge = var_f64_merge, .finalize = fin_stddev_f64, .finalize_value = fin_stddev_f64_value,
};

static const agg_vtable_t STDDEV_POP_F32_NATIVE = {
    .state_size = sizeof(var_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_f64_init, .update_batch = var_f32_native_update,
    .merge = var_f64_merge, .finalize = fin_stddev_pop_f64, .finalize_value = fin_stddev_pop_f64_value,
};

static void sum_f32_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const float* d = (const float*)vals;
    AGG_UPDATE_LOOP(valid, n,
        ((sum_f64_state*)((char*)base + (size_t)gids[i]*stride))->sum += d[i]);
}

static const agg_vtable_t SUM_F32_NATIVE = {
    .state_size = sizeof(sum_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = sum_f64_init, .update_batch = sum_f32_native_update,
    .merge = sum_f64_merge, .finalize = sum_f64_final, .finalize_value = sum_f64_final_value,
};

static void min_date_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->seen = 1;
    });
}

static bool ext_date_native_value(const void* s, void* dst) {
    const ext_i32_state* st = s;
    *(int32_t*)dst = st->seen ? (int32_t)st->v : NULL_I32;
    return !st->seen;
}
static ray_t* ext_date_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i32_state* st = s;
    return st->seen ? ray_date(st->v) : ray_typed_null(-RAY_DATE);
}

static const agg_vtable_t MIN_DATE_NATIVE = {
    SHARED_UPDATE(min_date_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_DATE,
    .init = min_i32_init, .update_batch = min_date_native_update,
    .merge = min_i32_merge, .finalize = ext_date_native_final, .finalize_value = ext_date_native_value,
};

static void max_date_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->seen = 1;
    });
}

static const agg_vtable_t MAX_DATE_NATIVE = {
    SHARED_UPDATE(max_date_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_DATE,
    .init = max_i32_init, .update_batch = max_date_native_update,
    .merge = max_i32_merge, .finalize = ext_date_native_final, .finalize_value = ext_date_native_value,
};

static void avg_date_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_DATE_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_date_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_date_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_DATE_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_date_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_DATE_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_date_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_DATE_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_date_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_DATE_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_date_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

static void min_time_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->seen = 1;
    });
}

static bool ext_time_native_value(const void* s, void* dst) {
    const ext_i32_state* st = s;
    *(int32_t*)dst = st->seen ? (int32_t)st->v : NULL_I32;
    return !st->seen;
}
static ray_t* ext_time_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i32_state* st = s;
    return st->seen ? ray_time(st->v) : ray_typed_null(-RAY_TIME);
}

static const agg_vtable_t MIN_TIME_NATIVE = {
    SHARED_UPDATE(min_time_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_TIME,
    .init = min_i32_init, .update_batch = min_time_native_update,
    .merge = min_i32_merge, .finalize = ext_time_native_final, .finalize_value = ext_time_native_value,
};

static void max_time_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i32_state* st = (ext_i32_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->seen = 1;
    });
}

static const agg_vtable_t MAX_TIME_NATIVE = {
    SHARED_UPDATE(max_time_shared)
    .state_size = sizeof(ext_i32_state), .kind = ACC_STREAMING, .out_type = RAY_TIME,
    .init = max_i32_init, .update_batch = max_time_native_update,
    .merge = max_i32_merge, .finalize = ext_time_native_final, .finalize_value = ext_time_native_value,
};

static void avg_time_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_TIME_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_time_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_time_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_TIME_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_time_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_TIME_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_time_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_TIME_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_time_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_TIME_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_time_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

static void sum_time_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* arena) {
    (void)arena;
    const int32_t* d = (const int32_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        sum_i64_state* st = (sum_i64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum = (int64_t)((uint64_t)st->sum + (uint64_t)d[i]); /* unsigned wrap: group.c:185 */
    });
}

static int64_t sum_time_native_final_result(const void* s) {
    return ((const sum_i64_state*)s)->sum;
}
AGG_SCALAR_FINAL(sum_time_native_final, int32_t, ray_time, value == NULL_I32)

static const agg_vtable_t SUM_TIME_NATIVE = {
    .state_size = sizeof(sum_i64_state), .kind = ACC_STREAMING, .out_type = RAY_TIME,
    .init = sum_i64_init, .update_batch = sum_time_native_update,
    .merge = sum_i64_merge, .finalize = sum_time_native_final, .finalize_value = sum_time_native_final_value,
};

static void min_timestamp_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] < st->v) st->v = d[i];
        st->cnt++;
    });
}

static bool ext_timestamp_native_value(const void* s, void* dst) {
    const ext_i64_state* st = s;
    *(int64_t*)dst = st->cnt ? (int64_t)st->v : NULL_I64;
    return !st->cnt;
}
static ray_t* ext_timestamp_native_final(const void* s, acc_arena_t* a, int64_t param) {
    (void)a; (void)param; const ext_i64_state* st = s;
    return st->cnt ? ray_timestamp(st->v) : ray_typed_null(-RAY_TIMESTAMP);
}

static const agg_vtable_t MIN_TIMESTAMP_NATIVE = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_TIMESTAMP,
    .init = min_i64_init, .update_batch = min_timestamp_native_update,
    .merge = min_i64_merge, .finalize = ext_timestamp_native_final, .finalize_value = ext_timestamp_native_value,
};

static void max_timestamp_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        ext_i64_state* st = (ext_i64_state*)((char*)base + (size_t)gids[i]*stride);
        if (d[i] > st->v) st->v = d[i];
        st->cnt++;
    });
}

static const agg_vtable_t MAX_TIMESTAMP_NATIVE = {
    .state_size = sizeof(ext_i64_state), .kind = ACC_STREAMING, .out_type = RAY_TIMESTAMP,
    .init = max_i64_init, .update_batch = max_timestamp_native_update,
    .merge = max_i64_merge, .finalize = ext_timestamp_native_final, .finalize_value = ext_timestamp_native_value,
};

static void avg_timestamp_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        avg_f64_state* st = (avg_f64_state*)((char*)base + (size_t)gids[i]*stride);
        st->sum += d[i]; st->cnt++;
    });
}

static const agg_vtable_t AVG_TIMESTAMP_NATIVE = {
    .state_size = sizeof(avg_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = avg_f64_init, .update_batch = avg_timestamp_native_update,
    .merge = avg_f64_merge, .finalize = avg_f64_final, .finalize_value = avg_f64_final_value,
};

static void var_timestamp_native_update(void* base, size_t stride, const uint32_t* gids,
                           const void* vals, const ray_valid_t* valid,
                           int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = (const int64_t*)vals;
    AGG_UPDATE_LOOP(valid, n, {
        var_i64_state* st = (var_i64_state*)((char*)base + (size_t)gids[i]*stride);
        int64_t v = d[i]; st->sum += (double)v;
        st->sumsq = (int64_t)((uint64_t)st->sumsq + (uint64_t)v*(uint64_t)v); /* wrap: group.c:185 */
        st->cnt++;
    });
}

static const agg_vtable_t VAR_TIMESTAMP_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_timestamp_native_update,
    .merge = var_i64_merge, .finalize = fin_var_i64, .finalize_value = fin_var_i64_value,
};

static const agg_vtable_t VAR_POP_TIMESTAMP_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_timestamp_native_update,
    .merge = var_i64_merge, .finalize = fin_var_pop_i64, .finalize_value = fin_var_pop_i64_value,
};

static const agg_vtable_t STDDEV_TIMESTAMP_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_timestamp_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_i64, .finalize_value = fin_stddev_i64_value,
};

static const agg_vtable_t STDDEV_POP_TIMESTAMP_NATIVE = {
    .state_size = sizeof(var_i64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = var_i64_init, .update_batch = var_timestamp_native_update,
    .merge = var_i64_merge, .finalize = fin_stddev_pop_i64, .finalize_value = fin_stddev_pop_i64_value,
};

typedef struct { int64_t sum, cnt; } prod_i64_state;
typedef struct { double sum; int64_t cnt; } prod_f64_state;
static int64_t prod_i64_final_result(const void* s) {
    const prod_i64_state* st = s;
    return st->cnt ? st->sum : NULL_I64;
}
AGG_SCALAR_FINAL(prod_i64_final, int64_t, ray_i64, value == NULL_I64)
static double prod_f64_final_result(const void* s) {
    const prod_f64_state* st = s;
    return st->cnt ? ray_f64_fin(st->sum) : NULL_F64;
}
AGG_SCALAR_FINAL(prod_f64_final, double, ray_f64, value != value)
static void prod_bool_init(void* s) { ((prod_i64_state*)s)->sum = 1; ((prod_i64_state*)s)->cnt = 0; }
static void prod_bool_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_i64_state* st = (prod_i64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = (int64_t)((uint64_t)st->sum * (uint64_t)d[i]);
    });
}
static void prod_bool_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_i64_state* dst = d; const prod_i64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = (int64_t)((uint64_t)dst->sum * (uint64_t)src->sum);
}
static const agg_vtable_t PROD_BOOL = {
    .state_size = sizeof(prod_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = prod_bool_init, .update_batch = prod_bool_update,
    .merge = prod_bool_merge, .finalize = prod_i64_final, .finalize_value = prod_i64_final_value,
};

static void prod_u8_init(void* s) { ((prod_i64_state*)s)->sum = 1; ((prod_i64_state*)s)->cnt = 0; }
static void prod_u8_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const uint8_t* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_i64_state* st = (prod_i64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = (int64_t)((uint64_t)st->sum * (uint64_t)d[i]);
    });
}
static void prod_u8_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_i64_state* dst = d; const prod_i64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = (int64_t)((uint64_t)dst->sum * (uint64_t)src->sum);
}
static const agg_vtable_t PROD_U8 = {
    .state_size = sizeof(prod_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = prod_u8_init, .update_batch = prod_u8_update,
    .merge = prod_u8_merge, .finalize = prod_i64_final, .finalize_value = prod_i64_final_value,
};

static void prod_i16_init(void* s) { ((prod_i64_state*)s)->sum = 1; ((prod_i64_state*)s)->cnt = 0; }
static void prod_i16_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const int16_t* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_i64_state* st = (prod_i64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = (int64_t)((uint64_t)st->sum * (uint64_t)d[i]);
    });
}
static void prod_i16_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_i64_state* dst = d; const prod_i64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = (int64_t)((uint64_t)dst->sum * (uint64_t)src->sum);
}
static const agg_vtable_t PROD_I16 = {
    .state_size = sizeof(prod_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = prod_i16_init, .update_batch = prod_i16_update,
    .merge = prod_i16_merge, .finalize = prod_i64_final, .finalize_value = prod_i64_final_value,
};

static void prod_i32_init(void* s) { ((prod_i64_state*)s)->sum = 1; ((prod_i64_state*)s)->cnt = 0; }
static void prod_i32_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const int32_t* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_i64_state* st = (prod_i64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = (int64_t)((uint64_t)st->sum * (uint64_t)d[i]);
    });
}
static void prod_i32_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_i64_state* dst = d; const prod_i64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = (int64_t)((uint64_t)dst->sum * (uint64_t)src->sum);
}
static const agg_vtable_t PROD_I32 = {
    .state_size = sizeof(prod_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = prod_i32_init, .update_batch = prod_i32_update,
    .merge = prod_i32_merge, .finalize = prod_i64_final, .finalize_value = prod_i64_final_value,
};

static void prod_i64_init(void* s) { ((prod_i64_state*)s)->sum = 1; ((prod_i64_state*)s)->cnt = 0; }
static void prod_i64_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const int64_t* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_i64_state* st = (prod_i64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = (int64_t)((uint64_t)st->sum * (uint64_t)d[i]);
    });
}
static void prod_i64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_i64_state* dst = d; const prod_i64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = (int64_t)((uint64_t)dst->sum * (uint64_t)src->sum);
}
static const agg_vtable_t PROD_I64 = {
    .state_size = sizeof(prod_i64_state), .kind = ACC_STREAMING, .out_type = RAY_I64,
    .init = prod_i64_init, .update_batch = prod_i64_update,
    .merge = prod_i64_merge, .finalize = prod_i64_final, .finalize_value = prod_i64_final_value,
};

static void prod_f32_init(void* s) { ((prod_f64_state*)s)->sum = 1; ((prod_f64_state*)s)->cnt = 0; }
static void prod_f32_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const float* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_f64_state* st = (prod_f64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = st->sum * d[i];
    });
}
static void prod_f32_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_f64_state* dst = d; const prod_f64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = dst->sum * src->sum;
}
static const agg_vtable_t PROD_F32 = {
    .state_size = sizeof(prod_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = prod_f32_init, .update_batch = prod_f32_update,
    .merge = prod_f32_merge, .finalize = prod_f64_final, .finalize_value = prod_f64_final_value,
};

static void prod_f64_init(void* s) { ((prod_f64_state*)s)->sum = 1; ((prod_f64_state*)s)->cnt = 0; }
static void prod_f64_update(void* base, size_t stride, const uint32_t* gids,
        const void* vals, const ray_valid_t* valid, int64_t n, acc_arena_t* a) {
    (void)a; const double* d = vals;
    AGG_UPDATE_LOOP(valid, n, {
        prod_f64_state* st = (prod_f64_state*)((char*)base + (size_t)gids[i] * stride);
        st->cnt++;
        st->sum = st->sum * d[i];
    });
}
static void prod_f64_merge(void* d, const void* s, acc_arena_t* a) {
    (void)a; prod_f64_state* dst = d; const prod_f64_state* src = s;
    dst->cnt += src->cnt;
    dst->sum = dst->sum * src->sum;
}
static const agg_vtable_t PROD_F64 = {
    .state_size = sizeof(prod_f64_state), .kind = ACC_STREAMING, .out_type = RAY_F64,
    .init = prod_f64_init, .update_batch = prod_f64_update,
    .merge = prod_f64_merge, .finalize = prod_f64_final, .finalize_value = prod_f64_final_value,
};

const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type) {
    if (agg_kind == OP_PROD && in_type == RAY_BOOL) return &PROD_BOOL;
    if (agg_kind == OP_PROD && in_type == RAY_U8) return &PROD_U8;
    if (agg_kind == OP_PROD && in_type == RAY_I16) return &PROD_I16;
    if (agg_kind == OP_PROD && in_type == RAY_I32) return &PROD_I32;
    if (agg_kind == OP_PROD && in_type == RAY_I64) return &PROD_I64;
    if (agg_kind == OP_PROD && in_type == RAY_F32) return &PROD_F32;
    if (agg_kind == OP_PROD && in_type == RAY_F64) return &PROD_F64;
    if (agg_kind == OP_MIN && in_type == RAY_BOOL) return &MIN_BOOL_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_BOOL) return &MAX_BOOL_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_BOOL) return &AVG_BOOL_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_BOOL) return &VAR_BOOL_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_BOOL) return &VAR_POP_BOOL_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_BOOL) return &STDDEV_BOOL_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_BOOL) return &STDDEV_POP_BOOL_NATIVE;
    if (agg_kind == OP_SUM && in_type == RAY_BOOL) return &SUM_BOOL_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_U8) return &MIN_U8_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_U8) return &MAX_U8_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_U8) return &AVG_U8_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_U8) return &VAR_U8_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_U8) return &VAR_POP_U8_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_U8) return &STDDEV_U8_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_U8) return &STDDEV_POP_U8_NATIVE;
    if (agg_kind == OP_SUM && in_type == RAY_U8) return &SUM_U8_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_I16) return &MIN_I16_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_I16) return &MAX_I16_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_I16) return &AVG_I16_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_I16) return &VAR_I16_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_I16) return &VAR_POP_I16_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_I16) return &STDDEV_I16_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_I16) return &STDDEV_POP_I16_NATIVE;
    if (agg_kind == OP_SUM && in_type == RAY_I16) return &SUM_I16_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_I32) return &MIN_I32_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_I32) return &MAX_I32_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_I32) return &AVG_I32_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_I32) return &VAR_I32_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_I32) return &VAR_POP_I32_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_I32) return &STDDEV_I32_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_I32) return &STDDEV_POP_I32_NATIVE;
    if (agg_kind == OP_SUM && in_type == RAY_I32) return &SUM_I32_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_I64) return &AVG_I64_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_F32) return &MIN_F32_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_F32) return &MAX_F32_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_F32) return &AVG_F32_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_F32) return &VAR_F32_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_F32) return &VAR_POP_F32_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_F32) return &STDDEV_F32_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_F32) return &STDDEV_POP_F32_NATIVE;
    if (agg_kind == OP_SUM && in_type == RAY_F32) return &SUM_F32_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_DATE) return &MIN_DATE_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_DATE) return &MAX_DATE_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_DATE) return &AVG_DATE_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_DATE) return &VAR_DATE_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_DATE) return &VAR_POP_DATE_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_DATE) return &STDDEV_DATE_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_DATE) return &STDDEV_POP_DATE_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_TIME) return &MIN_TIME_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_TIME) return &MAX_TIME_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_TIME) return &AVG_TIME_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_TIME) return &VAR_TIME_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_TIME) return &VAR_POP_TIME_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_TIME) return &STDDEV_TIME_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_TIME) return &STDDEV_POP_TIME_NATIVE;
    if (agg_kind == OP_SUM && in_type == RAY_TIME) return &SUM_TIME_NATIVE;
    if (agg_kind == OP_MIN && in_type == RAY_TIMESTAMP) return &MIN_TIMESTAMP_NATIVE;
    if (agg_kind == OP_MAX && in_type == RAY_TIMESTAMP) return &MAX_TIMESTAMP_NATIVE;
    if (agg_kind == OP_AVG && in_type == RAY_TIMESTAMP) return &AVG_TIMESTAMP_NATIVE;
    if (agg_kind == OP_VAR && in_type == RAY_TIMESTAMP) return &VAR_TIMESTAMP_NATIVE;
    if (agg_kind == OP_VAR_POP && in_type == RAY_TIMESTAMP) return &VAR_POP_TIMESTAMP_NATIVE;
    if (agg_kind == OP_STDDEV && in_type == RAY_TIMESTAMP) return &STDDEV_TIMESTAMP_NATIVE;
    if (agg_kind == OP_STDDEV_POP && in_type == RAY_TIMESTAMP) return &STDDEV_POP_TIMESTAMP_NATIVE;
    if (agg_kind == OP_TOP_N && in_type == RAY_I64) return &TOPK_I64;
    if (agg_kind == OP_BOT_N && in_type == RAY_I64) return &BOTK_I64;
    if (agg_kind == OP_TOP_N && in_type == RAY_F64) return &TOPK_F64;
    if (agg_kind == OP_BOT_N && in_type == RAY_F64) return &BOTK_F64;
    if (agg_kind == OP_MEDIAN && in_type == RAY_I64) return &MEDIAN_I64;
    if (agg_kind == OP_MEDIAN && in_type == RAY_F64) return &MEDIAN_F64;
    if (in_type == RAY_F32 || in_type == RAY_F64 || in_type == RAY_I64 || in_type == RAY_I32 ||
        in_type == RAY_I16 || in_type == RAY_U8  || in_type == RAY_BOOL) {
        if (agg_kind == OP_ALL) return &ALL_BOOL;
        if (agg_kind == OP_ANY) return &ANY_BOOL;
        if (agg_kind == OP_PEARSON_CORR) return &PEARSON_F64;
        if (agg_kind == OP_COV)  return &COV_F64;
        if (agg_kind == OP_SCOV) return &SCOV_F64;
        if (agg_kind == OP_WSUM) return &WSUM_F64;
        if (agg_kind == OP_WAVG) return &WAVG_F64;
    }
    if (agg_kind == OP_SUM && in_type == RAY_I64) return &SUM_I64;
    if (agg_kind == OP_COUNT)                     return &COUNT_ANY;
    if (agg_kind == OP_MIN && in_type == RAY_I64) return &MIN_I64;
    if (agg_kind == OP_MAX && in_type == RAY_I64) return &MAX_I64;
    if (agg_kind == OP_SUM && in_type == RAY_F64) return &SUM_F64;
    if (agg_kind == OP_MIN && in_type == RAY_F64) return &MIN_F64;
    if (agg_kind == OP_MAX && in_type == RAY_F64) return &MAX_F64;
    if (agg_kind == OP_AVG && in_type == RAY_F64) return &AVG_F64;
    if (in_type == RAY_I64) {
        if (agg_kind == OP_VAR) return &VAR_I64;
        if (agg_kind == OP_VAR_POP) return &VAR_POP_I64;
        if (agg_kind == OP_STDDEV) return &STDDEV_I64;
        if (agg_kind == OP_STDDEV_POP) return &STDDEV_POP_I64;
    } else if (in_type == RAY_F64) {
        if (agg_kind == OP_VAR) return &VAR_F64;
        if (agg_kind == OP_VAR_POP) return &VAR_POP_F64;
        if (agg_kind == OP_STDDEV) return &STDDEV_F64;
        if (agg_kind == OP_STDDEV_POP) return &STDDEV_POP_F64;
    }
    return NULL;
}
