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

#include "dict.h"
#include "table.h"
#include "table/sym.h"
#include "lang/internal.h"   /* atom_eq for RAY_LIST key compares */
#include <string.h>

/* --------------------------------------------------------------------------
 * Layout
 *
 *   Block header (32B) | slot[0] = keys (ray_t*) | slot[1] = vals (ray_t*)
 *
 *   d->type   = RAY_DICT
 *   d->len    = 2 (slot count, kept consistent with table block convention)
 *   keys: any vector type; pair count = keys->len
 *   vals: typed vector when homogeneous, RAY_LIST otherwise
 * -------------------------------------------------------------------------- */

#define DICT_DATA_SIZE  (2 * sizeof(ray_t*))

static ray_t* dict_alloc_block(ray_t* keys, ray_t* vals) {
    ray_t* d = ray_alloc(DICT_DATA_SIZE);
    if (!d || RAY_IS_ERR(d)) return d;
    d->type  = RAY_DICT;
    d->attrs = 0;
    d->len   = 2;
    memset(d->nullmap, 0, 16);
    ray_t** slots = ray_dict_slots(d);
    slots[0] = keys;
    slots[1] = vals;
    return d;
}

/* --------------------------------------------------------------------------
 * ray_dict_new — wrap two refs into a fresh RAY_DICT block.
 *
 * Ownership: consumes one ref each of `keys` and `vals` (transferred into
 * the dict).  On error, both refs are released.  Returns rc=1 dict.
 * -------------------------------------------------------------------------- */

ray_t* ray_dict_new(ray_t* keys, ray_t* vals) {
    if (!keys || RAY_IS_ERR(keys)) {
        if (vals && !RAY_IS_ERR(vals)) ray_release(vals);
        return keys ? keys : ray_error("type", NULL);
    }
    if (!vals || RAY_IS_ERR(vals)) {
        ray_release(keys);
        return vals ? vals : ray_error("type", NULL);
    }
    ray_t* d = dict_alloc_block(keys, vals);
    if (!d || RAY_IS_ERR(d)) {
        ray_release(keys);
        ray_release(vals);
        return d ? d : ray_error("oom", NULL);
    }
    return d;
}

/* --------------------------------------------------------------------------
 * ray_dict_keys / ray_dict_vals — borrowed pointers; do not release.
 * -------------------------------------------------------------------------- */

ray_t* ray_dict_keys(ray_t* d) {
    if (!d || RAY_IS_ERR(d) || d->type != RAY_DICT) return NULL;
    return ray_dict_slots(d)[0];
}

ray_t* ray_dict_vals(ray_t* d) {
    if (!d || RAY_IS_ERR(d) || d->type != RAY_DICT) return NULL;
    return ray_dict_slots(d)[1];
}

int64_t ray_dict_len(ray_t* d) {
    ray_t* keys = ray_dict_keys(d);
    return keys ? keys->len : 0;
}

/* --------------------------------------------------------------------------
 * ray_dict_find_sym — fast sym-only probe (no atom boxing).
 * -------------------------------------------------------------------------- */

int64_t ray_dict_find_sym(ray_t* d, int64_t sym_id) {
    if (!d || RAY_IS_ERR(d) || d->type != RAY_DICT) return -1;
    ray_t* keys = ray_dict_slots(d)[0];
    if (!keys || RAY_IS_ERR(keys) || keys->type != RAY_SYM) return -1;
    void* base = ray_data(keys);
    int64_t n = keys->len;
    uint8_t aw = keys->attrs & RAY_SYM_W_MASK;
    switch (aw) {
        case RAY_SYM_W8: {
            const uint8_t* a = (const uint8_t*)base;
            for (int64_t i = 0; i < n; i++) if ((int64_t)a[i] == sym_id) return i;
            return -1;
        }
        case RAY_SYM_W16: {
            const uint16_t* a = (const uint16_t*)base;
            for (int64_t i = 0; i < n; i++) if ((int64_t)a[i] == sym_id) return i;
            return -1;
        }
        case RAY_SYM_W32: {
            const uint32_t* a = (const uint32_t*)base;
            for (int64_t i = 0; i < n; i++) if ((int64_t)a[i] == sym_id) return i;
            return -1;
        }
        default: {
            const int64_t* a = (const int64_t*)base;
            for (int64_t i = 0; i < n; i++) if (a[i] == sym_id) return i;
            return -1;
        }
    }
}

ray_t* ray_dict_probe_sym_borrowed(ray_t* d, int64_t sym_id) {
    int64_t idx = ray_dict_find_sym(d, sym_id);
    if (idx < 0) return NULL;
    ray_t* vals = ray_dict_slots(d)[1];
    if (!vals || RAY_IS_ERR(vals) || vals->type != RAY_LIST) return NULL;
    return ((ray_t**)ray_data(vals))[idx];
}

ray_t* ray_container_probe_sym(ray_t* v, int64_t sym_id) {
    if (!v || RAY_IS_ERR(v)) return NULL;
    if (v->type == RAY_DICT) return ray_dict_probe_sym_borrowed(v, sym_id);
    if (v->type == RAY_TABLE) return ray_table_get_col(v, sym_id);
    return NULL;
}

/* --------------------------------------------------------------------------
 * ray_dict_find_idx — locate key index in keys vector; -1 if missing.
 *
 * Dispatches on keys->type; the key atom must have matching atom type
 * (e.g. -RAY_SYM key for a RAY_SYM keys vector).  Returns -1 on type
 * mismatch rather than erroring; the caller surfaces the error.
 * -------------------------------------------------------------------------- */

int64_t ray_dict_find_idx(ray_t* d, ray_t* key_atom) {
    if (!d || RAY_IS_ERR(d) || d->type != RAY_DICT) return -1;
    if (!key_atom || RAY_IS_ERR(key_atom)) return -1;

    ray_t* keys = ray_dict_slots(d)[0];
    if (!keys || RAY_IS_ERR(keys)) return -1;
    int8_t kt = keys->type;
    int64_t n = keys->len;
    if (n <= 0) return -1;

    /* RAY_LIST keys: heterogeneous, compare via atom_eq. */
    if (kt == RAY_LIST) {
        ray_t** ks = (ray_t**)ray_data(keys);
        for (int64_t i = 0; i < n; i++)
            if (atom_eq(ks[i], key_atom)) return i;
        return -1;
    }

    /* Typed-vector keys: atom type must match. */
    if (key_atom->type != -kt) return -1;

    /* Null-aware probe: a null key atom matches only null slots; a non-null
     * key atom must match a non-null slot of equal value.  Without this, a
     * group dict containing both `0Nl` and `0` keys (now produced as
     * distinct buckets by ray_group_fn) would still resolve `(at d 0Nl)`
     * to the first non-null zero — re-introducing the conflation we just
     * fixed in grouping. */
    bool key_is_null = RAY_ATOM_IS_NULL(key_atom);
    bool keys_have_nulls = (keys->attrs & RAY_ATTR_HAS_NULLS) != 0
                            || (keys->attrs & RAY_ATTR_SLICE);
    if (key_is_null) {
        if (!keys_have_nulls) return -1;
        for (int64_t i = 0; i < n; i++)
            if (ray_vec_is_null(keys, i)) return i;
        return -1;
    }

    void* base = ray_data(keys);
#define DICT_FIND_LOOP(EQ_EXPR) do {                              \
        if (keys_have_nulls) {                                    \
            for (int64_t i = 0; i < n; i++) {                     \
                if (ray_vec_is_null(keys, i)) continue;           \
                if (EQ_EXPR) return i;                            \
            }                                                     \
        } else {                                                  \
            for (int64_t i = 0; i < n; i++)                       \
                if (EQ_EXPR) return i;                            \
        }                                                         \
        return -1;                                                \
    } while (0)

    switch (kt) {
        case RAY_SYM: {
            int64_t key_id = key_atom->i64;
            uint8_t aw = keys->attrs;
            switch (aw & RAY_SYM_W_MASK) {
                case RAY_SYM_W8: {
                    const uint8_t* a = (const uint8_t*)base;
                    DICT_FIND_LOOP((int64_t)a[i] == key_id);
                }
                case RAY_SYM_W16: {
                    const uint16_t* a = (const uint16_t*)base;
                    DICT_FIND_LOOP((int64_t)a[i] == key_id);
                }
                case RAY_SYM_W32: {
                    const uint32_t* a = (const uint32_t*)base;
                    DICT_FIND_LOOP((int64_t)a[i] == key_id);
                }
                default: {
                    const int64_t* a = (const int64_t*)base;
                    DICT_FIND_LOOP(a[i] == key_id);
                }
            }
        }
        case RAY_I64:
        case RAY_TIMESTAMP: {
            const int64_t* a = (const int64_t*)base;
            int64_t v = key_atom->i64;
            DICT_FIND_LOOP(a[i] == v);
        }
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME: {
            const int32_t* a = (const int32_t*)base;
            int32_t v = key_atom->i32;
            DICT_FIND_LOOP(a[i] == v);
        }
        case RAY_I16: {
            const int16_t* a = (const int16_t*)base;
            int16_t v = key_atom->i16;
            DICT_FIND_LOOP(a[i] == v);
        }
        case RAY_BOOL:
        case RAY_U8: {
            const uint8_t* a = (const uint8_t*)base;
            uint8_t v = key_atom->u8;
            DICT_FIND_LOOP(a[i] == v);
        }
        case RAY_F32: {
            const float* a = (const float*)base;
            float v = (float)key_atom->f64;
            DICT_FIND_LOOP(a[i] == v);
        }
        case RAY_F64: {
            const double* a = (const double*)base;
            double v = key_atom->f64;
            DICT_FIND_LOOP(a[i] == v);
        }
        case RAY_STR: {
            const char* kp = ray_str_ptr(key_atom);
            size_t klen = ray_str_len(key_atom);
            for (int64_t i = 0; i < n; i++) {
                if (keys_have_nulls && ray_vec_is_null(keys, i)) continue;
                size_t vlen = 0;
                const char* vp = ray_str_vec_get(keys, i, &vlen);
                if (!vp) continue;
                if (vlen == klen && (klen == 0 || memcmp(vp, kp, klen) == 0))
                    return i;
            }
            return -1;
        }
        case RAY_GUID: {
            const uint8_t* a = (const uint8_t*)base;
            const uint8_t* kp = key_atom->obj ? (const uint8_t*)ray_data(key_atom->obj) : NULL;
            if (!kp) return -1;
            for (int64_t i = 0; i < n; i++) {
                if (keys_have_nulls && ray_vec_is_null(keys, i)) continue;
                if (memcmp(a + i * 16, kp, 16) == 0) return i;
            }
            return -1;
        }
        default:
            return -1;
    }
#undef DICT_FIND_LOOP
}

/* --------------------------------------------------------------------------
 * Internal: read element at index out of a vals container as a borrowed
 * ray_t*.  For RAY_LIST that's a stored pointer.  For typed vectors we
 * synthesize a fresh atom (rc=1) — caller owns the returned ref.
 *
 * `*owned_out` is set true if the caller must release the result, false if
 * it's borrowed (must NOT be released by the caller).
 * -------------------------------------------------------------------------- */

static ray_t* dict_vals_at(ray_t* vals, int64_t idx, bool* owned_out) {
    *owned_out = false;
    if (!vals || RAY_IS_ERR(vals)) return NULL;
    if (idx < 0 || idx >= vals->len) return NULL;

    if (vals->type == RAY_LIST) {
        ray_t** slots = (ray_t**)ray_data(vals);
        return slots[idx];
    }

    /* Typed vector — box element into a fresh atom so the caller has a
     * uniform ray_t* contract.  Mark as owned so the caller releases. */
    ray_t* atom = NULL;
    void* base = ray_data(vals);
    switch (vals->type) {
        case RAY_BOOL:      atom = ray_bool(((uint8_t*)base)[idx]);                  break;
        case RAY_U8:        atom = ray_u8(((uint8_t*)base)[idx]);                    break;
        case RAY_I16:       atom = ray_i16(((int16_t*)base)[idx]);                   break;
        case RAY_I32:       atom = ray_i32(((int32_t*)base)[idx]);                   break;
        case RAY_I64:       atom = ray_i64(((int64_t*)base)[idx]);                   break;
        case RAY_F32:       atom = ray_f32(((float*)base)[idx]);                     break;
        case RAY_F64:       atom = ray_f64(((double*)base)[idx]);                    break;
        case RAY_DATE:      atom = ray_date(((int32_t*)base)[idx]);                  break;
        case RAY_TIME:      atom = ray_time(((int32_t*)base)[idx]);                  break;
        case RAY_TIMESTAMP: atom = ray_timestamp(((int64_t*)base)[idx]);              break;
        case RAY_SYM: {
            int64_t id = ray_read_sym(base, idx, vals->type, vals->attrs);
            atom = ray_sym(id);
            break;
        }
        case RAY_STR: {
            size_t slen = 0;
            const char* sp = ray_str_vec_get(vals, idx, &slen);
            atom = sp ? ray_str(sp, slen) : ray_str("", 0);
            break;
        }
        case RAY_GUID:
            atom = ray_guid(((uint8_t*)base) + idx * 16);
            break;
        default:
            return NULL;
    }
    if (atom && !RAY_IS_ERR(atom)) *owned_out = true;
    return atom;
}

/* --------------------------------------------------------------------------
 * ray_dict_get — return value for `key_atom`, or NULL if missing.
 *
 * The returned pointer is owned by the caller (rc=1) — callers must
 * `ray_release` it after use.  This makes the contract uniform whether
 * vals is a typed vector (boxed atom) or a RAY_LIST (retained slot).
 * -------------------------------------------------------------------------- */

ray_t* ray_dict_get(ray_t* d, ray_t* key_atom) {
    int64_t i = ray_dict_find_idx(d, key_atom);
    if (i < 0) return NULL;
    ray_t* vals = ray_dict_slots(d)[1];
    bool owned = false;
    ray_t* out = dict_vals_at(vals, i, &owned);
    if (!out || RAY_IS_ERR(out)) return out;
    if (!owned) ray_retain(out);
    return out;
}

/* --------------------------------------------------------------------------
 * promote_vals_to_list — return a RAY_LIST equivalent to `vals`.
 *
 * If `vals` is already a RAY_LIST we return it unchanged (borrowed).  If
 * `vals` is a typed vector we materialize each element into a fresh atom
 * inside a new RAY_LIST — the caller owns the new list and must release
 * it (and the original `vals` separately if it owns that).
 *
 * Used by upsert/remove to keep mutation paths simple regardless of the
 * incoming vals shape.
 * -------------------------------------------------------------------------- */

static ray_t* promote_vals_to_list(ray_t* vals) {
    if (!vals || RAY_IS_ERR(vals)) return vals;
    if (vals->type == RAY_LIST) {
        ray_retain(vals);
        return vals;
    }
    int64_t n = vals->len;
    ray_t* lst = ray_list_new(n);
    if (!lst || RAY_IS_ERR(lst)) return lst ? lst : ray_error("oom", NULL);
    for (int64_t i = 0; i < n; i++) {
        bool owned = false;
        ray_t* a = dict_vals_at(vals, i, &owned);
        if (!a || RAY_IS_ERR(a)) {
            ray_release(lst);
            return a ? a : ray_error("oom", NULL);
        }
        ray_t* lst2 = ray_list_append(lst, a);
        if (owned) ray_release(a);
        if (!lst2 || RAY_IS_ERR(lst2)) {
            if (lst2 == NULL) ray_release(lst);
            return lst2 ? lst2 : ray_error("oom", NULL);
        }
        lst = lst2;
    }
    return lst;
}

/* --------------------------------------------------------------------------
 * ray_dict_upsert — set d[key_atom] = val.
 *
 * Ownership: consumes `d`; on success the ref is transferred into the
 * returned dict (rc=1).  On error `d` is released.  Does NOT consume
 * `key_atom` or `val` — both are retained internally as needed.
 *
 * Existing-key fast path: COW the dict, replace val slot in place when
 * vals is a RAY_LIST; if vals is a typed vector matching val's atom type
 * we COW vals and rewrite the element; otherwise promote vals to RAY_LIST
 * first.  Missing-key path: append key & val (always promoting vals to
 * RAY_LIST so the homogeneous typed-vec invariant is not silently broken).
 * -------------------------------------------------------------------------- */

ray_t* ray_dict_upsert(ray_t* d, ray_t* key_atom, ray_t* val) {
    if (!d || RAY_IS_ERR(d)) return d ? d : ray_error("type", NULL);
    if (!val || RAY_IS_ERR(val)) {
        ray_release(d);
        return val ? val : ray_error("type", NULL);
    }

    /* Empty-target special case: build a fresh dict.  Keys vector type
     * mirrors the key atom's atom type. */
    if (d->type != RAY_DICT) {
        ray_release(d);
        if (!key_atom || RAY_IS_ERR(key_atom)) return ray_error("type", NULL);
        int8_t kt = (int8_t)-key_atom->type;
        ray_t* keys = (kt == RAY_SYM)
            ? ray_sym_vec_new(RAY_SYM_W64, 1)
            : ray_vec_new(kt, 1);
        if (!keys || RAY_IS_ERR(keys)) return keys ? keys : ray_error("oom", NULL);
        ray_t* vals = ray_list_new(1);
        if (!vals || RAY_IS_ERR(vals)) { ray_release(keys); return vals ? vals : ray_error("oom", NULL); }
        ray_t* d2 = ray_dict_new(keys, vals);
        if (!d2 || RAY_IS_ERR(d2)) return d2;
        return ray_dict_upsert(d2, key_atom, val);
    }

    int64_t idx = ray_dict_find_idx(d, key_atom);

    /* COW the dict; the slots remain shared until we COW them too. */
    d = ray_cow(d);
    if (!d || RAY_IS_ERR(d)) return d;

    ray_t** slots = ray_dict_slots(d);
    ray_t* keys = slots[0];
    ray_t* vals = slots[1];

    /* The append/set helpers consume the input ref and return an owned ref
     * (possibly the same pointer, or a fresh one after grow — in which case
     * the OLD block is already freed inside the helper).  So we always
     * overwrite slots[*] with the helper return and never release the old
     * pointer ourselves — that would double-free on grow. */
    if (idx >= 0) {
        /* Replace existing slot. */
        if (vals->type == RAY_LIST) {
            ray_t* new_vals = ray_list_set(vals, idx, val);
            if (!new_vals || RAY_IS_ERR(new_vals)) { ray_release(d); return new_vals ? new_vals : ray_error("oom", NULL); }
            slots[1] = new_vals;
        } else {
            /* Typed vector path: promote to LIST first, then update. */
            ray_t* lst = promote_vals_to_list(vals);
            if (!lst || RAY_IS_ERR(lst)) { ray_release(d); return lst ? lst : ray_error("oom", NULL); }
            ray_release(vals);
            slots[1] = lst;
            ray_t* new_lst = ray_list_set(lst, idx, val);
            if (!new_lst || RAY_IS_ERR(new_lst)) { ray_release(d); return new_lst ? new_lst : ray_error("oom", NULL); }
            slots[1] = new_lst;
        }
        return d;
    }

    /* Missing key — append to both vectors.  Promote vals to LIST first. */
    if (vals->type != RAY_LIST) {
        ray_t* lst = promote_vals_to_list(vals);
        if (!lst || RAY_IS_ERR(lst)) { ray_release(d); return lst ? lst : ray_error("oom", NULL); }
        ray_release(vals);
        slots[1] = lst;
        vals = lst;
    }

    /* Append key — helper consumes `keys`, returns owned (possibly new) ref. */
    ray_t* new_keys = NULL;
    if (keys->type == RAY_SYM) {
        int64_t kid = key_atom->i64;
        new_keys = ray_vec_append(keys, &kid);
    } else if (keys->type == RAY_STR && key_atom->type == -RAY_STR) {
        new_keys = ray_str_vec_append(keys, ray_str_ptr(key_atom), ray_str_len(key_atom));
    } else if (keys->type == RAY_GUID && key_atom->type == -RAY_GUID) {
        const void* src = key_atom->obj ? ray_data(key_atom->obj) : NULL;
        if (!src) { ray_release(d); return ray_error("type", NULL); }
        new_keys = ray_vec_append(keys, src);
    } else if (keys->type == RAY_F32 && key_atom->type == -RAY_F32) {
        /* F32 atoms keep their value in the f64 union slot; the keys vec
         * stores narrower 4-byte floats, so narrow before append (the
         * generic &u8 fallback below would copy the wrong half of the
         * double bit pattern). */
        float f = (float)key_atom->f64;
        new_keys = ray_vec_append(keys, &f);
    } else if (keys->type == -key_atom->type) {
        new_keys = ray_vec_append(keys, &key_atom->u8);
    } else {
        ray_release(d);
        return ray_error("type", NULL);
    }
    if (!new_keys || RAY_IS_ERR(new_keys)) { ray_release(d); return new_keys ? new_keys : ray_error("oom", NULL); }
    slots[0] = new_keys;

    /* Append val — list_append consumes vals, returns owned (possibly new). */
    ray_t* new_vals = ray_list_append(vals, val);
    if (!new_vals || RAY_IS_ERR(new_vals)) { ray_release(d); return new_vals ? new_vals : ray_error("oom", NULL); }
    slots[1] = new_vals;

    return d;
}

/* --------------------------------------------------------------------------
 * ray_dict_remove — drop the (key, val) pair if present.
 *
 * Ownership: consumes `d`; transferred into the returned dict.  If the
 * key isn't present, returns the input unchanged (one-ref transferred).
 * -------------------------------------------------------------------------- */

ray_t* ray_dict_remove(ray_t* d, ray_t* key_atom) {
    if (!d || RAY_IS_ERR(d)) return d ? d : ray_error("type", NULL);
    if (d->type != RAY_DICT) { ray_release(d); return ray_error("type", NULL); }

    int64_t idx = ray_dict_find_idx(d, key_atom);
    if (idx < 0) return d;

    d = ray_cow(d);
    if (!d || RAY_IS_ERR(d)) return d;

    ray_t** slots = ray_dict_slots(d);
    ray_t* keys = slots[0];
    ray_t* vals = slots[1];

    /* Promote typed-vector vals to LIST so we can remove uniformly. */
    if (vals->type != RAY_LIST) {
        ray_t* lst = promote_vals_to_list(vals);
        if (!lst || RAY_IS_ERR(lst)) { ray_release(d); return lst ? lst : ray_error("oom", NULL); }
        ray_release(vals);
        slots[1] = lst;
        vals = lst;
    }

    /* Drop key element by slicing (build a smaller vec without idx). */
    int64_t n = keys->len;
    ray_t* new_keys = NULL;
    if (keys->type == RAY_SYM) {
        new_keys = ray_sym_vec_new(keys->attrs & RAY_SYM_W_MASK, n - 1);
    } else if (keys->type == RAY_STR) {
        new_keys = ray_vec_new(RAY_STR, n - 1);
    } else {
        new_keys = ray_vec_new(keys->type, n - 1);
    }
    if (!new_keys || RAY_IS_ERR(new_keys)) { ray_release(d); return new_keys ? new_keys : ray_error("oom", NULL); }

    /* Copy keys[0..idx-1] then keys[idx+1..n-1] into new_keys. */
    if (keys->type == RAY_STR) {
        for (int64_t i = 0; i < n; i++) {
            if (i == idx) continue;
            size_t slen = 0;
            const char* sp = ray_str_vec_get(keys, i, &slen);
            ray_t* nk = ray_str_vec_append(new_keys, sp ? sp : "", sp ? slen : 0);
            if (!nk || RAY_IS_ERR(nk)) { ray_release(new_keys); ray_release(d); return nk ? nk : ray_error("oom", NULL); }
            new_keys = nk;
        }
    } else if (keys->type == RAY_SYM) {
        uint8_t aw = keys->attrs & RAY_SYM_W_MASK;
        void* sb = ray_data(keys);
        for (int64_t i = 0; i < n; i++) {
            if (i == idx) continue;
            int64_t sid = ray_read_sym(sb, i, RAY_SYM, aw);
            ray_t* nk = ray_vec_append(new_keys, &sid);
            if (!nk || RAY_IS_ERR(nk)) { ray_release(new_keys); ray_release(d); return nk ? nk : ray_error("oom", NULL); }
            new_keys = nk;
        }
    } else {
        uint8_t esz = ray_sym_elem_size(keys->type, keys->attrs);
        const uint8_t* base = (const uint8_t*)ray_data(keys);
        for (int64_t i = 0; i < n; i++) {
            if (i == idx) continue;
            ray_t* nk = ray_vec_append(new_keys, base + (size_t)i * esz);
            if (!nk || RAY_IS_ERR(nk)) { ray_release(new_keys); ray_release(d); return nk ? nk : ray_error("oom", NULL); }
            new_keys = nk;
        }
    }
    slots[0] = new_keys;
    ray_release(keys);

    /* Drop vals[idx] from the LIST. */
    vals = ray_cow(vals);
    if (!vals || RAY_IS_ERR(vals)) { ray_release(d); return vals; }
    slots[1] = vals;
    ray_t** vslots = (ray_t**)ray_data(vals);
    if (vslots[idx]) ray_release(vslots[idx]);
    for (int64_t i = idx; i + 1 < vals->len; i++) vslots[i] = vslots[i + 1];
    vals->len -= 1;

    return d;
}
