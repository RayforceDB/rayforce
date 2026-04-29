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

#include "linkop.h"
#include "idxop.h"
#include "ops/internal.h"   /* col_propagate_str_pool */
#include "ops/ops.h"        /* RAY_IS_PARTED */
#include "mem/cow.h"
#include "vec/vec.h"
#include "table/table.h"
#include "table/sym.h"
#include "lang/eval.h"
#include "lang/env.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * Promote inline nullmap to ext-nullmap before attaching a link.
 *
 * A linked column places its int64 target sym at nullmap-union bytes 8-15.
 * If the column has inline nulls and >64 elements, those bytes hold real
 * bitmap bits that would be clobbered.  Promote up front to keep nulls
 * intact.  Mirrors the promotion logic in ray_vec_set_null_checked. */
static ray_err_t promote_inline_to_ext(ray_t* vec) {
    if (!(vec->attrs & RAY_ATTR_HAS_NULLS)) return RAY_OK;
    if (vec->attrs & RAY_ATTR_NULLMAP_EXT)  return RAY_OK;

    int64_t bitmap_len = (vec->len + 7) / 8;
    if (bitmap_len < 1) bitmap_len = 1;
    ray_t* ext = ray_vec_new(RAY_U8, bitmap_len);
    if (!ext || RAY_IS_ERR(ext)) return RAY_ERR_OOM;
    ext->len = bitmap_len;

    /* Copy existing inline bits (16 bytes max) into ext. */
    int64_t copy = bitmap_len < 16 ? bitmap_len : 16;
    memcpy(ray_data(ext), vec->nullmap, (size_t)copy);
    if (bitmap_len > 16) {
        memset((char*)ray_data(ext) + 16, 0, (size_t)(bitmap_len - 16));
    }
    /* Now overwrite bytes 0-7 with the ext_nullmap pointer.  Bytes 8-15
     * become don't-care — caller is about to write link_target there. */
    vec->ext_nullmap = ext;
    vec->attrs |= RAY_ATTR_NULLMAP_EXT;
    return RAY_OK;
}

/* --------------------------------------------------------------------------
 * ray_link_attach
 * -------------------------------------------------------------------------- */

ray_t* ray_link_attach(ray_t** vp, int64_t target_sym_id) {
    if (!vp || !*vp || RAY_IS_ERR(*vp))
        return ray_error("type", "link: null/error vector");
    ray_t* v = *vp;

    if (!ray_is_vec(v) || (v->type != RAY_I32 && v->type != RAY_I64))
        return ray_error("type", "link: column must be RAY_I32 or RAY_I64 (got %d)",
                         (int)v->type);
    if (v->attrs & RAY_ATTR_SLICE)
        return ray_error("type", "link: cannot attach to a slice; materialize first");
    if (target_sym_id < 0)
        return ray_error("type", "link: invalid target sym ID");

    /* Validate that target_sym_id resolves to a RAY_TABLE in the env. */
    ray_t* target = ray_env_get(target_sym_id);
    if (!target || target->type != RAY_TABLE)
        return ray_error("name", "link: target sym does not name a table");

    /* Reject parted dim tables — deref math (target_col[linkcol[i]]) is
     * straight indexing, with no notion of which segment a global rowid
     * lives in.  Pointing a link at a parted target would silently
     * misbehave at deref time.  Better an explicit nyi here than a
     * three-layers-deep wrong-answer bug.  See guide-indexes.html and
     * queries-links.html for the supported shape (parted fact -> regular
     * non-parted dim). */
    int64_t tcols = ray_table_ncols(target);
    for (int64_t c = 0; c < tcols; c++) {
        ray_t* tcol = ray_table_get_col_idx(target, c);
        if (tcol && RAY_IS_PARTED(tcol->type))
            return ray_error("nyi",
                "link: target table has a parted column (%d); "
                "link targets must be non-parted (in-memory or splayed) tables",
                (int)c);
    }

    /* COW so we own the bytes we're about to mutate. */
    v = ray_cow(v);
    if (!v || RAY_IS_ERR(v)) return v;
    *vp = v;

    /* Promote nulls to ext if necessary so bytes 8-15 are free. */
    ray_err_t err = promote_inline_to_ext(v);
    if (err != RAY_OK) return ray_error(ray_err_code_str(err), "link: oom");

    /* Replace any existing link (idempotent re-attach with new target). */
    v->link_target = target_sym_id;
    v->attrs |= RAY_ATTR_HAS_LINK;

    /* If an accelerator index is also attached, the index's saved snapshot
     * captured the pre-link bytes 8-15 (which were _idx_pad / NULL).  Update
     * the snapshot so a future index-drop restores the link too. */
    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        ray_index_t* ix = ray_index_payload(v->index);
        memcpy(&ix->saved_nullmap[8], &target_sym_id, 8);
    }
    return v;
}

/* --------------------------------------------------------------------------
 * ray_link_detach
 * -------------------------------------------------------------------------- */

ray_t* ray_link_detach(ray_t** vp) {
    if (!vp || !*vp || RAY_IS_ERR(*vp)) return *vp;
    ray_t* v = *vp;
    if (!(v->attrs & RAY_ATTR_HAS_LINK)) return v;

    v = ray_cow(v);
    if (!v || RAY_IS_ERR(v)) { *vp = v; return v; }
    *vp = v;

    v->link_target = 0;
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_LINK;

    if (v->attrs & RAY_ATTR_HAS_INDEX) {
        ray_index_t* ix = ray_index_payload(v->index);
        memset(&ix->saved_nullmap[8], 0, 8);
    }
    return v;
}

/* --------------------------------------------------------------------------
 * ray_link_deref — produce target_col[link_col[i]] for each row i
 *
 * Result type matches the target column type.  Length matches the link
 * column.  Null rows in the link become null in the result; null rows in
 * the target also propagate.  Returns NULL when target table or field
 * column don't exist (caller treats as a probe miss).
 * -------------------------------------------------------------------------- */

ray_t* ray_link_deref(ray_t* v, int64_t sym_id) {
    if (!ray_link_has(v)) return NULL;
    if (v->type != RAY_I32 && v->type != RAY_I64) return NULL;

    /* Slice-through: a slice over a linked parent inherits the link.
     * link_target lives on the parent; the slice's own bytes 8-15 are
     * slice_offset, which would be garbage if we read it as a sym ID. */
    int64_t target_sym = (v->attrs & RAY_ATTR_SLICE)
                         ? v->slice_parent->link_target
                         : v->link_target;
    ray_t* target_tab = ray_env_get(target_sym);
    if (!target_tab || target_tab->type != RAY_TABLE) return NULL;

    /* Reject parted targets at deref time, mirroring the attach-time guard
     * in ray_link_attach.  The attach-time check catches the obvious case
     * (user calls (.col.link 'parted_dim ...)), but two paths bypass it:
     *   1. Lazy rebind — attach saw a non-parted table; the sym was later
     *      rebound to a parted one (env lookup is at deref time).
     *   2. .link sidecar reload — try_load_link_sidecar (col.c) writes
     *      link_target straight from the on-disk sym name without any
     *      env-state check.
     * Without a deref-time guard, both produce a silent wrong-answer bug
     * (target_col[linkcol[i]] indexes into RAY_PARTED data, which is a
     * list of segment pointers — straight-byte indexing is meaningless). */
    int64_t tcols = ray_table_ncols(target_tab);
    for (int64_t c = 0; c < tcols; c++) {
        ray_t* tcol = ray_table_get_col_idx(target_tab, c);
        if (tcol && RAY_IS_PARTED(tcol->type))
            return ray_error("nyi",
                "link deref: target table has a parted column (%d); "
                "links to parted dim tables are not supported in v1",
                (int)c);
    }

    ray_t* target_col = ray_table_get_col(target_tab, sym_id);
    if (!target_col) return NULL;

    int64_t n = v->len;
    int64_t target_n = target_col->len;
    int8_t  out_type = target_col->type;

    /* Resolve through slices: SYM-width and (later) sym_dict / str_pool
     * all live on the slice_parent's attrs/union, never on the slice
     * itself.  The slice contributes only its [slice_offset, len) view.
     * Compute the canonical width and base-pointer once here so the
     * gather loop stays correct for narrow-width sliced sym columns. */
    ray_t* col_owner = (target_col->attrs & RAY_ATTR_SLICE)
                       ? target_col->slice_parent : target_col;
    int64_t col_off  = (target_col->attrs & RAY_ATTR_SLICE)
                       ? target_col->slice_offset : 0;
    uint8_t target_width = col_owner->attrs & RAY_SYM_W_MASK;
    uint8_t target_esz   = (out_type == RAY_SYM)
                           ? (uint8_t)(1u << target_width)
                           : ray_sym_elem_size(out_type, col_owner->attrs);

    /* Allocate result.  For RAY_SYM mirror the parent's width so the
     * subsequent memcpy is byte-correct; otherwise the canonical size
     * for the type. */
    ray_t* result;
    if (out_type == RAY_SYM) {
        result = ray_sym_vec_new(target_width, n);
    } else {
        result = ray_vec_new(out_type, n);
    }
    if (!result || RAY_IS_ERR(result)) return result;
    result->len = n;

    uint8_t out_esz = ray_sym_elem_size(out_type, result->attrs);
    if (out_esz > 0) memset(ray_data(result), 0, (size_t)n * out_esz);
    /* By construction, out_esz == target_esz: SYM widths match,
     * STR is always 16, numeric types match because out_type == target. */

    const uint8_t* link_base = (const uint8_t*)ray_data(v);
    uint8_t link_esz = ray_sym_elem_size(v->type, v->attrs);
    char* out_base = (char*)ray_data(result);
    /* Compute the source-data base by hand (not via ray_data on the
     * slice) because ray_data_fn assumes ray_type_sizes[RAY_SYM] = 8
     * (W64), which mis-offsets narrow-width sliced sym columns. */
    const char* col_data_base = (const char*)ray_data(col_owner);
    const char* tgt_base      = col_data_base + (size_t)col_off * target_esz;

    for (int64_t i = 0; i < n; i++) {
        if (ray_vec_is_null(v, i)) {
            ray_vec_set_null(result, i, true);
            continue;
        }
        int64_t rid;
        if (link_esz == 4) {
            int32_t r;
            memcpy(&r, link_base + i * 4, 4);
            rid = (int64_t)r;
        } else {
            memcpy(&rid, link_base + i * 8, 8);
        }
        if (rid < 0 || rid >= target_n) {
            ray_vec_set_null(result, i, true);
            continue;
        }
        if (ray_vec_is_null(target_col, rid)) {
            ray_vec_set_null(result, i, true);
            continue;
        }
        if (target_esz > 0 && out_esz == target_esz) {
            memcpy(out_base + i * out_esz,
                   tgt_base + rid * target_esz,
                   target_esz);
        }
    }

    /* Type-specific metadata propagation.
     *   RAY_STR: share the source pool so ray_str_t pool_offs are valid.
     *   RAY_SYM: if the source column carries a local sym_dict, share it.
     *
     * sym_dict aliases bytes 8-15 of the nullmap union.  It is only a
     * real pointer when the column doesn't have inline nulls clobbering
     * those bytes, i.e. either no nulls or NULLMAP_EXT.  Mirrors the
     * guard pattern in src/ops/sort.c:3307 and src/ops/rerank.c:182. */
    if (out_type == RAY_STR) {
        col_propagate_str_pool(result, target_col);
    } else if (out_type == RAY_SYM) {
        if (col_owner && !(col_owner->attrs & RAY_ATTR_SLICE) &&
            (!(col_owner->attrs & RAY_ATTR_HAS_NULLS) ||
             (col_owner->attrs & RAY_ATTR_NULLMAP_EXT)) &&
            col_owner->sym_dict) {
            ray_retain(col_owner->sym_dict);
            result->sym_dict = col_owner->sym_dict;
        }
    }
    return result;
}

/* --------------------------------------------------------------------------
 * Rayfall builtin entry points
 * -------------------------------------------------------------------------- */

ray_t* ray_col_link_fn(ray_t* target_sym, ray_t* int_vec) {
    if (!target_sym || target_sym->type != -RAY_SYM)
        return ray_error("type", "(.col.link target v): target must be a sym");
    if (!int_vec || RAY_IS_ERR(int_vec))
        return int_vec ? int_vec : ray_error("type", "(.col.link target v): null v");
    int64_t target_id = target_sym->i64;

    ray_t* w = int_vec;
    ray_retain(w);
    ray_t* r = ray_link_attach(&w, target_id);
    if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    return w;
}

ray_t* ray_col_unlink_fn(ray_t* v) {
    if (!v || RAY_IS_ERR(v)) return v;
    ray_t* w = v;
    ray_retain(w);
    ray_t* r = ray_link_detach(&w);
    if (RAY_IS_ERR(r)) { ray_release(w); return r; }
    return w;
}

ray_t* ray_col_link_p_fn(ray_t* v) {
    return ray_bool(ray_link_has(v) ? 1 : 0);
}

ray_t* ray_col_target_fn(ray_t* v) {
    if (!ray_link_has(v)) return RAY_NULL_OBJ;
    /* Slice-aware: ray_link_target_id reads from slice_parent for slices,
     * because v->link_target on a slice aliases slice_offset and would
     * surface as a garbage sym ID. */
    return ray_sym(ray_link_target_id(v));
}
