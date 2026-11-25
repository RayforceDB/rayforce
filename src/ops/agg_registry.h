/* src/ops/agg_registry.h — resolve (agg_kind, in_type) → accumulator vtable.
 * agg_kind values are the existing OP_SUM/OP_MIN/... opcodes from ops/ops.h. */
#ifndef RAY_OPS_AGG_REGISTRY_H
#define RAY_OPS_AGG_REGISTRY_H

#include "ops/agg_acc.h"

/* Returns NULL when no specialized vtable is registered for (agg_kind, in_type).
 * In later phases NULL routes to the buffered-eval oracle accumulator. */
const agg_vtable_t* agg_resolve(uint16_t agg_kind, int8_t in_type);

/* A 64-bit integer variance that may have wrapped its difference (#1670):
 * the finalizers and merges bump the count, and exec_group_v2 then runs the
 * group again resolving through agg_resolve_checked (agg_stream.c). */
#include <stdatomic.h>
#include <stdint.h>
extern _Atomic uint64_t ray_var_wrap_suspects;
const agg_vtable_t* agg_resolve_checked(uint16_t agg_kind, int8_t in_type);

#endif /* RAY_OPS_AGG_REGISTRY_H */
