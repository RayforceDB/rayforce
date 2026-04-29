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

#ifndef RAY_LINKOP_H
#define RAY_LINKOP_H

/*
 * linkop.h -- Linked columns.
 *
 * A linked column is an integer vector (RAY_I32 / RAY_I64) where every
 * value is a row index into a target table.  Querying linkcol.field
 * dereferences as target_table[linkcol[i]][field] for each row i — a
 * single array access, no hash probe.
 *
 * Storage: RAY_ATTR_HAS_LINK = 0x04 set on the column; the int64 sym ID
 * naming the target lives at bytes 8-15 of the nullmap union (the
 * `link_target` field).  See include/rayforce.h for the union layout
 * and src/mem/heap.h for the attr-bit semantics.
 *
 * Resolution is lazy: link_target is just a sym, looked up against the
 * global env at deref time.  If the target table has been rebound, the
 * link follows automatically.
 *
 * HAS_LINK is a property of the column, not a transient accelerator —
 * unlike HAS_INDEX it is preserved across in-place mutation and
 * persisted to disk via a `.link` sidecar file.
 */

#include <rayforce.h>
#include "mem/heap.h"

/* ===== Attach / Detach ===== */

/* Attach a link to *vp pointing at the target named by target_sym_id.
 * Returns the (possibly COW'd) parent vector with HAS_LINK set, or a
 * RAY_ERROR.  Validates: target sym must resolve to a RAY_TABLE in the
 * current env; *vp must be a RAY_I32 or RAY_I64 vector and not a slice. */
ray_t* ray_link_attach(ray_t** vp, int64_t target_sym_id);

/* Clear HAS_LINK from *vp.  No-op if not linked.  link_target byte slot
 * is zeroed.  Returns *vp. */
ray_t* ray_link_detach(ray_t** vp);

/* ===== Introspection ===== */

/* True iff `v` is a linked column or a slice of one.  Slices over a
 * linked parent inherit the link transparently — the slice's own attrs
 * carry RAY_ATTR_SLICE without HAS_LINK, but `link_target` lives on the
 * parent and reading it through the slice is safe via slice_parent. */
static inline bool ray_link_has(const ray_t* v) {
    if (!v || RAY_IS_ERR((ray_t*)v)) return false;
    if (v->attrs & RAY_ATTR_HAS_LINK) return true;
    if (v->attrs & RAY_ATTR_SLICE) {
        const ray_t* p = v->slice_parent;
        return p && (p->attrs & RAY_ATTR_HAS_LINK);
    }
    return false;
}

/* Returns the target sym ID (int64) or -1 if no link is attached.
 * Slice-aware: looks through to slice_parent->link_target. */
static inline int64_t ray_link_target_id(const ray_t* v) {
    if (!ray_link_has(v)) return (int64_t)-1;
    if (v->attrs & RAY_ATTR_SLICE) return v->slice_parent->link_target;
    return v->link_target;
}

/* ===== Resolution ===== */

/* Dereference linked column v at field sym_id of the target table.
 * Returns a fresh owning ref to a column the same length as v, with
 * the same type as the target's field column.  Null rows in v
 * propagate as nulls in the result; null rows in the target also
 * propagate.  Returns NULL if the target table is missing or doesn't
 * have a column named `sym_id` (caller may treat as a probe miss). */
ray_t* ray_link_deref(ray_t* v, int64_t sym_id);

/* ===== Rayfall builtin entry points ===== */

ray_t* ray_col_link_fn   (ray_t* target_sym, ray_t* int_vec);  /* (.col.link 'target v) */
ray_t* ray_col_unlink_fn (ray_t* v);                            /* (.col.unlink v) */
ray_t* ray_col_link_p_fn (ray_t* v);                            /* (.col.link? v) */
ray_t* ray_col_target_fn (ray_t* v);                            /* (.col.target v) */

#endif /* RAY_LINKOP_H */
