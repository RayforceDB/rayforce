/* src/ops/agg_acc.h — composable accumulator interface (design §4).
 * Aggregates are referenced ONLY through this vtable; adding an aggregate
 * is registering one vtable, never a new operator + gates. */
#ifndef RAY_OPS_AGG_ACC_H
#define RAY_OPS_AGG_ACC_H

#include <rayforce.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* Per-column validity view, derived ONCE by the engine and honored by every
 * accumulator (design §4.6, §6 option A). Sentinel representation is unchanged;
 * what changes is that null detection is centralized here, not attr-gated per
 * kernel. has_nulls=false means the no-null fast path is provable for this batch. */
typedef struct {
    const void* base;     /* column data base pointer */
    int8_t      type;     /* RAY_I64 / RAY_F64 / ... */
    bool        has_nulls;
} ray_valid_t;

/* True iff row is a live (non-null) value. Mirrors the sentinel checks in
 * group.c REDUCE_LOOP_I/REDUCE_LOOP_F. */
static inline bool ray_valid_at(const ray_valid_t* v, int64_t row) {
    if (!v->has_nulls) return true;
    switch (v->type) {
        case RAY_I64: case RAY_TIMESTAMP:
            return ((const int64_t*)v->base)[row] != NULL_I64;
        case RAY_I32: case RAY_DATE: case RAY_TIME:
            return ((const int32_t*)v->base)[row] != NULL_I32;
        case RAY_I16:
            return ((const int16_t*)v->base)[row] != NULL_I16;
        case RAY_F32: {
            float d = ((const float*)v->base)[row];
            return d == d;
        }
        case RAY_F64: {
            double d = ((const double*)v->base)[row];
            return d == d;  /* only NaN fails self-equality */
        }
        default:
            return true;    /* BOOL/U8 etc. are non-nullable */
    }
}

/* Per-group side storage for ACC_BUFFERED accumulators (median/top-k).
 * Opaque + unused in Phase 0 — only streaming accumulators ship here. */
typedef struct acc_arena acc_arena_t;

typedef enum { ACC_STREAMING, ACC_BUFFERED } acc_kind_t;

typedef struct {
    uint16_t   state_size;   /* bytes of inline per-group state */
    acc_kind_t kind;
    int8_t     out_type;     /* result column/atom type */
    void   (*init)        (void* state);
    /* For row i in [0,n): apply vals[i] to group gids[i].
     * The group's inline state is at states_base + gids[i]*stride. */
    void   (*update_batch)(void* states_base, size_t stride,
                           const uint32_t* gids, const void* vals,
                           const ray_valid_t* valid, int64_t n,
                           acc_arena_t* arena);
    /* Binary aggregates (e.g. pearson): two value columns. NULL for unary
     * accumulators (the engine calls update_batch2 when non-NULL). A row
     * contributes only if BOTH x and y are valid. */
    void   (*update_batch2)(void* states_base, size_t stride, const uint32_t* gids,
                            const void* vals_x, const void* vals_y,
                            const ray_valid_t* valid_x, const ray_valid_t* valid_y,
                            int64_t n, acc_arena_t* arena);
    void   (*merge)       (void* dst, const void* src, acc_arena_t* arena);
    /* param: per-aggregate integer parameter (K for top_n/bot_n via ext->agg_k[a];
     * 0 and ignored for all other aggregates). */
    ray_t* (*finalize)    (const void* state, acc_arena_t* arena, int64_t param);
    /* Native scalar emit: supplied by every registered streaming kernel,
     * optional for buffered kernels. Writes one payload, returns whether null. */
    bool (*finalize_value)(const void* state, void* dst);
    /* Optional concurrent update of the same initialized state. Init and
     * finalize still run outside the update dispatch. The state layout and
     * null semantics must match update_batch; no separately owned memory. */
    void (*update_shared)(void* states_base, size_t stride, const uint32_t* gids,
                          const void* vals, const ray_valid_t* valid, int64_t n);
    /* Release heap state owned by a per-group state slab. NULL for ACC_STREAMING.
     * The engine calls this on every init'd state once it won't be used again. */
    void   (*destroy)(void* state);
} agg_vtable_t;

#endif /* RAY_OPS_AGG_ACC_H */
