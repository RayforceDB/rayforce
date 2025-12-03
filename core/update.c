/*
 *   Copyright (c) 2023 Anton Kundenko <singaraiona@gmail.com>
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

#include "update.h"
#include "runtime.h"
#include "error.h"
#include "eval.h"
#include "filter.h"
#include "binary.h"
#include "vary.h"
#include "index.h"
#include "items.h"
#include "group.h"
#include "filter.h"
#include "query.h"
#include "aggr.h"
#include "compose.h"

#define UNCOW_OBJ(o, v, orig, r) \
    {                            \
        if ((v == NULL)) {       \
            if (o != orig)       \
                drop_obj(o);     \
        } else if (*v != o)      \
            drop_obj(o);         \
        return r;                \
    };

obj_p __fetch(obj_p obj, obj_p **val, obj_p *original) {
    if (obj->type == -TYPE_SYMBOL) {
        *val = resolve(obj->i64);
        if (*val == NULL)
            THROW(ERR_NOT_FOUND, "fetch: symbol not found");

        *original = **val;
        obj = cow_obj(**val);
    } else {
        *val = NULL;
        *original = obj;
        obj = cow_obj(obj);
    }

    return obj;
}

obj_p __commit(obj_p src, obj_p obj, obj_p *val) {
    // TODO: comparison val and *obj in case of shrink is only guaranteed
    // to be correct with own allocator
    if (src->type == -TYPE_SYMBOL) {
        if ((val != NULL) && (*val != obj)) {
            drop_obj(*val);
            *val = obj;
        }

        return clone_obj(src);
    }

    return (src == obj) ? clone_obj(obj) : obj;
}

b8_t __suitable_types(obj_p x, obj_p y) {
    i8_t yt;

    if (y->type < 0)
        yt = -y->type;
    else
        yt = y->type;

    if ((x->type != TYPE_LIST) && (x->type != TYPE_MAPLIST) && (x->type != yt) &&
        (x->type != TYPE_ENUM && yt != TYPE_SYMBOL)) {
        return B8_FALSE;
    }

    return B8_TRUE;
}

b8_t __suitable_lengths(obj_p x, obj_p y) {
    if (x->type != TYPE_LIST && (IS_VECTOR(y) && (ops_count(y) != ops_count(x))))
        return B8_FALSE;

    return B8_TRUE;
}

/*
 * Helper to find the index of a symbol in a symbol vector.
 * Returns the index or NULL_I64 if not found.
 */
i64_t __find_symbol_idx(obj_p symbols, i64_t sym) {
    i64_t i, l;
    i64_t *syms;

    l = symbols->len;
    syms = AS_I64(symbols);

    for (i = 0; i < l; i++) {
        if (syms[i] == sym)
            return i;
    }

    return NULL_I64;
}

/*
 * Reorder input values to match the table's column order.
 * Handles both single records (list of atoms) and multiple records (list of vectors).
 *
 * table_cols: symbol vector of table column names
 * table_vals: list of table column data (to get types for null values)
 * input_cols: symbol vector of input column names
 * input_vals: list of input values
 *
 * Returns: reordered list matching table column order, or error object
 */
obj_p __reorder_columns(obj_p table_cols, obj_p table_vals, obj_p input_cols, obj_p input_vals) {
    i64_t i, j, tc, ic, rec_count;
    i8_t col_type;
    obj_p reordered;
    i64_t *tcols, *icols;
    b8_t is_single_record;

    tc = table_cols->len;  // number of table columns
    ic = input_cols->len;  // number of input columns
    tcols = AS_I64(table_cols);
    icols = AS_I64(input_cols);

    // Check that all input columns exist in the table
    for (i = 0; i < ic; i++) {
        if (__find_symbol_idx(table_cols, icols[i]) == NULL_I64) {
            return ray_error(ERR_NOT_FOUND, "column '%s' not found in table", str_from_symbol(icols[i]));
        }
    }

    // Determine if this is a single record or multiple records
    is_single_record = IS_ATOM(AS_LIST(input_vals)[0]);

    if (is_single_record) {
        // Single record: create a reordered list of atoms
        reordered = LIST(tc);

        for (i = 0; i < tc; i++) {
            // Find this table column in the input
            j = __find_symbol_idx(input_cols, tcols[i]);

            if (j == NULL_I64) {
                // Column not in input - use null with the correct type from table
                col_type = AS_LIST(table_vals)[i]->type;
                AS_LIST(reordered)[i] = null(col_type);
            } else {
                AS_LIST(reordered)[i] = clone_obj(AS_LIST(input_vals)[j]);
            }
        }
    } else {
        // Multiple records: create a reordered list of vectors
        rec_count = AS_LIST(input_vals)[0]->len;
        reordered = LIST(tc);

        for (i = 0; i < tc; i++) {
            // Find this table column in the input
            j = __find_symbol_idx(input_cols, tcols[i]);

            if (j == NULL_I64) {
                // Column not in input - use null vector with the correct type
                col_type = AS_LIST(table_vals)[i]->type;
                AS_LIST(reordered)[i] = nullv(col_type, rec_count);
            } else {
                AS_LIST(reordered)[i] = clone_obj(AS_LIST(input_vals)[j]);
            }
        }
    }

    return reordered;
}

obj_p *at_obj_ref(obj_p obj, obj_p idx) {
    i64_t j;
    obj_p v, *p;

    switch (MTYPE2(obj->type, idx->type)) {
        case MTYPE2(TYPE_LIST, -TYPE_I64):
            return &AS_LIST(obj)[idx->i64];
        default:
            if (obj->type == TYPE_DICT) {
                j = find_obj_idx(AS_LIST(obj)[0], idx);
                if (j == NULL_I64)
                    return NULL;
                else {
                    if (AS_LIST(obj)[1]->type != TYPE_LIST)
                        diverse_obj(AS_LIST(obj) + 1);

                    p = AS_LIST(AS_LIST(obj)[1]) + j;
                    v = cow_obj(*p);
                    if (v != *p) {
                        drop_obj(*p);
                        *p = v;
                    }

                    return p;
                }
            }

            return NULL;
    }
}

obj_p dot_obj(obj_p obj, obj_p idx) {
    i64_t i, l;
    obj_p *ref;

    switch (idx->type) {
        case TYPE_NULL:
            return obj;
        case TYPE_LIST:
            l = idx->len;
            if (l < 2)
                THROW(ERR_NOT_FOUND, "dot: invalid index len");

            l--;  // skip last element

            for (i = 0; i < l; i++) {
                obj = cow_obj(obj);
                obj = dot_obj(obj, AS_LIST(idx)[i]);
                if (obj == NULL)
                    THROW(ERR_NOT_FOUND, "dot: invalid index");
            }

            return obj;

        default:
            ref = at_obj_ref(cow_obj(obj), idx);
            if (ref == NULL)
                THROW(ERR_NOT_FOUND, "dot: invalid index");
            return *ref;
    }
}

obj_p __alter(obj_p *obj, obj_p func, obj_p idx, obj_p val) {
    obj_p v, *p, args[3], res;

    // special case for set/let
    if (func->type == TYPE_BINARY && (func->i64 == (i64_t)ray_set || func->i64 == (i64_t)ray_let))
        return set_obj(obj, idx, clone_obj(val));

    // special case for concat
    if (func->type == TYPE_BINARY && func->i64 == (i64_t)ray_concat) {
        if (idx->type == TYPE_NULL)
            return push_obj(obj, clone_obj(val));

        p = at_obj_ref(*obj, idx);
        if (p == NULL)
            return ray_error(ERR_NOT_FOUND, "alter: invalid index");
        if (IS_ERR(*p))
            return *p;

        return push_obj(p, clone_obj(val));
    }

    // special case for remove
    if (func->type == TYPE_BINARY && func->i64 == (i64_t)ray_remove)
        return remove_obj(obj, val);

    // retrieve the object via indices
    v = at_obj(*obj, idx);

    if (IS_ERR(v))
        return v;

    args[0] = func;
    args[1] = v;
    args[2] = val;

    res = ray_apply(args, 3);
    drop_obj(v);

    if (IS_ERR(res))
        return res;

    return set_obj(obj, idx, res);
}

obj_p ray_alter(obj_p *x, i64_t n) {
    obj_p obj, res, *cur = NULL;

    if (n < 3)
        THROW(ERR_LENGTH, "alter: expected at least 3 arguments");

    if (x[1]->type < TYPE_LAMBDA || x[1]->type > TYPE_VARY)
        THROW(ERR_TYPE, "alter: expected function as 2nd argument");

    if (x[0]->type == -TYPE_SYMBOL) {
        cur = resolve(x[0]->i64);
        if (cur == NULL)
            THROW(ERR_NOT_FOUND, "alter: undefined symbol");
        obj = cow_obj(*cur);
    } else {
        obj = cow_obj(x[0]);
    }

    res = obj;
    res = (n == 4) ? __alter(&res, x[1], x[2], x[3]) : __alter(&res, x[1], NULL_OBJ, x[2]);

    if (IS_ERR(res)) {
        if (x[0]->type != -TYPE_SYMBOL)
            drop_obj(obj);

        return res;
    }

    if (x[0]->type != -TYPE_SYMBOL)
        return res;

    // If the object was reallocated (e.g., by push_obj), update the reference
    // Note: we don't drop *cur if obj == *cur originally, since they're the same object
    // and res is already pointing to the (possibly reallocated) version
    if (obj != *cur) {
        // obj was a copy, so drop the original
        drop_obj(*cur);
        *cur = res;
    } else if (res != obj) {
        // obj was not a copy (cow returned same object), but it got reallocated
        // So just update the pointer without dropping
        *cur = res;
    }

    return clone_obj(x[0]);
}

obj_p ray_modify(obj_p *x, i64_t n) {
    obj_p obj, res, *cur, idx;

    cur = NULL;

    if (n < 4)
        THROW(ERR_LENGTH, "modify: expected at least 4 arguments, got %lld", n);

    if (x[1]->type < TYPE_LAMBDA || x[1]->type > TYPE_VARY)
        THROW(ERR_TYPE, "modify: expected function as 2nd argument, got '%s'", type_name(x[1]->type));

    if (x[0]->type == -TYPE_SYMBOL) {
        cur = resolve(x[0]->i64);
        if (cur == NULL)
            THROW(ERR_NOT_FOUND, "modify: undefined symbol");
        obj = cow_obj(*cur);
    } else {
        obj = cow_obj(x[0]);
    }

    res = dot_obj(obj, x[2]);
    if (IS_ERR(res))
        return res;

    idx = ray_last(x[2]);
    if (IS_ERR(idx))
        return idx;

    res = __alter(&res, x[1], idx, x[3]);
    drop_obj(idx);

    if (IS_ERR(res))
        return res;

    if (x[0]->type != -TYPE_SYMBOL)
        return obj;

    if (cur != NULL && (*cur) != obj) {
        drop_obj(*cur);
        *cur = obj;
    }

    return clone_obj(x[0]);
}

/*
 * inserts for tables
 */
#define INSERT_ERROR(r)                   \
    {                                     \
        if (lst_allocated)                \
            drop_obj(lst);                \
        UNCOW_OBJ(obj, val, original, r); \
    }

obj_p ray_insert(obj_p *x, i64_t n) {
    i64_t i, m, l;
    obj_p lst, col, *val = NULL, obj, res, original;
    b8_t need_drop, lst_allocated;

    if (n != 2)
        THROW(ERR_LENGTH, "insert: expected 2 arguments, got %lld", n);

    obj = __fetch(x[0], &val, &original);

    if (IS_ERR(obj))
        return obj;

    if (obj->type != TYPE_TABLE) {
        res = ray_error(ERR_TYPE, "insert: expected 'Table as 1st argument, got '%s", type_name(obj->type));
        UNCOW_OBJ(obj, val, original, res);
    }

    // Check integrity of the table with the new object
    lst = x[1];
    lst_allocated = B8_FALSE;

    // Handle dict/table input: reorder columns to match table order
    if (lst->type == TYPE_DICT || lst->type == TYPE_TABLE) {
        if (lst->type == TYPE_DICT && AS_LIST(lst)[0]->type != TYPE_SYMBOL) {
            res = ray_error(ERR_TYPE, "insert: expected 'Symbol as 1st element in a dictionary, got '%s'",
                            type_name(AS_LIST(lst)[0]->type));
            UNCOW_OBJ(obj, val, original, res);
        }

        // Check that input doesn't have more columns than table
        l = AS_LIST(lst)[0]->len;
        if (l > AS_LIST(obj)[0]->len) {
            res = ray_error(ERR_LENGTH, "insert: input has more columns (%lld) than table (%lld)", l,
                            AS_LIST(obj)[0]->len);
            UNCOW_OBJ(obj, val, original, res);
        }

        // Reorder columns to match table order (allows flexible column ordering)
        res = __reorder_columns(AS_LIST(obj)[0], AS_LIST(obj)[1], AS_LIST(lst)[0], AS_LIST(lst)[1]);
        if (IS_ERR(res))
            UNCOW_OBJ(obj, val, original, res);

        lst = res;
        lst_allocated = B8_TRUE;
    }

    switch (lst->type) {
        case TYPE_LIST:
            l = lst->len;
            // With column reordering, we might have fewer columns than the table
            // but not more. The missing columns should be filled with nulls.
            if (l > AS_LIST(obj)[0]->len) {
                res = ray_error(ERR_LENGTH, "insert: expected list of length at most %lld, got %lld",
                                AS_LIST(obj)[0]->len, l);
                INSERT_ERROR(res);
            }

            // There is one record to be inserted
            if (IS_ATOM(AS_LIST(lst)[0])) {
                // Check all the elements of the list
                for (i = 0; i < l; i++) {
                    if (!__suitable_types(AS_LIST(AS_LIST(obj)[1])[i], AS_LIST(lst)[i])) {
                        res = ray_error(ERR_TYPE, "insert: expected '%s' as %lldth element in a values list, got '%s'",
                                        type_name(-AS_LIST(AS_LIST(obj)[1])[i]->type), i,
                                        type_name(AS_LIST(lst)[i]->type));
                        INSERT_ERROR(res);
                    }
                }

                // Insert the record now
                for (i = 0; i < l; i++) {
                    col = cow_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                    need_drop = (col != AS_LIST(AS_LIST(obj)[1])[i]);
                    res = push_obj(&col, clone_obj(AS_LIST(lst)[i]));
                    if (IS_ERR(res)) {
                        if (need_drop)
                            drop_obj(col);
                        INSERT_ERROR(res);
                    }
                    if (need_drop)
                        drop_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                    AS_LIST(AS_LIST(obj)[1])[i] = col;
                }
            } else {
                // There are multiple records to be inserted
                m = AS_LIST(lst)[0]->len;
                if (m == 0) {
                    res = ray_error(ERR_LENGTH, "insert: expected non-empty list of records");
                    INSERT_ERROR(res);
                }

                // Check all the elements of the list
                for (i = 0; i < l; i++) {
                    if (!__suitable_types(AS_LIST(AS_LIST(obj)[1])[i], AS_LIST(lst)[i])) {
                        res = ray_error(ERR_TYPE, "insert: expected '%s' as %lldth element, got '%s'",
                                        type_name(AS_LIST(AS_LIST(obj)[1])[i]->type), i,
                                        type_name(AS_LIST(lst)[i]->type));
                        INSERT_ERROR(res);
                    }

                    if (AS_LIST(lst)[i]->len != m) {
                        res = ray_error(ERR_LENGTH,
                                        "insert: expected list of length %lld, as %lldth element in a values, got %lld",
                                        AS_LIST(AS_LIST(obj)[1])[i]->len, i, n);
                        INSERT_ERROR(res);
                    }
                }

                // Insert all the records now
                for (i = 0; i < l; i++) {
                    col = cow_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                    need_drop = (col != AS_LIST(AS_LIST(obj)[1])[i]);
                    res = append_list(&col, AS_LIST(lst)[i]);
                    if (IS_ERR(res)) {
                        if (need_drop)
                            drop_obj(col);
                        INSERT_ERROR(res);
                    }
                    if (need_drop)
                        drop_obj(AS_LIST(AS_LIST(obj)[1])[i]);

                    AS_LIST(AS_LIST(obj)[1])[i] = col;
                }
            }

            break;

        default:
            res = ray_error(ERR_TYPE, "insert: unsupported type '%s' as 2nd argument", type_name(lst->type));
            INSERT_ERROR(res);
    }

    if (lst_allocated)
        drop_obj(lst);

    return __commit(x[0], obj, val);
}
#undef INSERT_ERROR

/*
 * update/inserts for tables
 */
#define UPSERT_ERROR(r)    \
    {                      \
        if (lst_allocated) \
            drop_obj(lst); \
        drop_obj(obj);     \
        return r;          \
    }

obj_p ray_upsert(obj_p *x, i64_t n) {
    i64_t i, j, m, p, l, ll, keys;
    i64_t row, *rows;
    obj_p obj, k1, k2, idx, col, lst, *val = NULL, v, original, res;
    b8_t lst_allocated;

    if (n != 3)
        THROW(ERR_LENGTH, "upsert: expected 3 arguments, got %lld", n);

    if (x[1]->type != -TYPE_I64)
        THROW(ERR_TYPE, "upsert: expected 'I64 as 2nd argument, got '%s'", type_name(x[1]->type));

    keys = x[1]->i64;
    if (keys < 1)
        THROW(ERR_LENGTH, "upsert: expected positive number of keys > 0, got %lld", keys);

    obj = __fetch(x[0], &val, &original);
    if (IS_ERR(obj))
        return obj;

    if (obj->type != TYPE_TABLE) {
        drop_obj(obj);
        return ray_error(ERR_TYPE, "upsert: expected 'Table as 1st argument, got '%s'", type_name(obj->type));
    }

    p = AS_LIST(obj)[0]->len;

    lst = x[2];
    lst_allocated = B8_FALSE;

    // Handle dict/table input: reorder columns to match table order
    if (lst->type == TYPE_DICT || lst->type == TYPE_TABLE) {
        if (lst->type == TYPE_DICT && AS_LIST(lst)[0]->type != TYPE_SYMBOL) {
            drop_obj(obj);
            return ray_error(ERR_TYPE, "upsert: expected 'Symbol as keys in a dictionary, got '%s'",
                             type_name(AS_LIST(lst)[0]->type));
        }

        // Check that input doesn't have more columns than table
        l = AS_LIST(lst)[0]->len;
        if (l > p) {
            drop_obj(obj);
            return ray_error(ERR_LENGTH, "upsert: input has more columns (%lld) than table (%lld)", l, p);
        }

        // Reorder columns to match table order (allows flexible column ordering)
        res = __reorder_columns(AS_LIST(obj)[0], AS_LIST(obj)[1], AS_LIST(lst)[0], AS_LIST(lst)[1]);
        if (IS_ERR(res)) {
            drop_obj(obj);
            return res;
        }

        lst = res;
        lst_allocated = B8_TRUE;
    }

    switch (lst->type) {
        case TYPE_LIST:
            l = ops_count(lst);

            if (l == 0)
                UPSERT_ERROR(ray_error(ERR_LENGTH, "upsert: expected non-empty list of records"));

            if (l > p)
                UPSERT_ERROR(
                    ray_error(ERR_LENGTH, "upsert: list length %lld is greater than table columns %lld", l, p));

            // Check if this is a single record (atoms) or multiple records (vectors)
            if (IS_ATOM(AS_LIST(lst)[0])) {
                // Single record case - validate all elements are atoms or compatible
                for (i = 0; i < l; i++) {
                    if (!__suitable_types(AS_LIST(AS_LIST(obj)[1])[i], AS_LIST(lst)[i])) {
                        UPSERT_ERROR(ray_error(ERR_TYPE, "upsert: expected '%s' as %lldth element, got '%s'",
                                               type_name(-AS_LIST(AS_LIST(obj)[1])[i]->type), i,
                                               type_name(AS_LIST(lst)[i]->type)));
                    }
                }

                // Build key for lookup
                if (keys == 1) {
                    k1 = at_idx(AS_LIST(obj)[1], 0);
                    k2 = clone_obj(AS_LIST(lst)[0]);
                } else {
                    k1 = ray_take(x[1], AS_LIST(obj)[1]);
                    k2 = ray_take(x[1], lst);
                }

                idx = index_upsert_obj(k2, k1, x[1]->i64);

                drop_obj(k1);
                drop_obj(k2);

                if (IS_ERR(idx))
                    UPSERT_ERROR(idx);

                row = AS_I64(idx)[0];
                drop_obj(idx);

                // Process each column
                for (i = 0; i < p; i++) {
                    col = cow_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                    if (col != AS_LIST(AS_LIST(obj)[1])[i]) {
                        drop_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                        AS_LIST(AS_LIST(obj)[1])[i] = col;
                    }

                    // Insert record
                    if (row == NULL_I64) {
                        v = (i < l) ? clone_obj(AS_LIST(lst)[i]) : null(AS_LIST(AS_LIST(obj)[1])[i]->type);
                        push_obj(AS_LIST(AS_LIST(obj)[1]) + i, v);
                    }
                    // Update record (skip keys since they match, and only update provided columns)
                    else if (i >= keys && i < l) {
                        set_idx(AS_LIST(AS_LIST(obj)[1]) + i, row, clone_obj(AS_LIST(lst)[i]));
                    }
                }

                if (lst_allocated)
                    drop_obj(lst);
                return __commit(x[0], obj, val);
            }

            // Multiple records case - validate the list contains vectors
            ll = AS_LIST(lst)[0]->len;
            for (i = 0; i < l; i++) {
                if (!IS_VECTOR(AS_LIST(lst)[i]))
                    UPSERT_ERROR(ray_error(ERR_TYPE, "upsert: expected vector as %lldth element of a list, got '%s'", i,
                                           type_name(AS_LIST(lst)[i]->type)));

                if (AS_LIST(lst)[i]->len != ll)
                    UPSERT_ERROR(ray_error(ERR_LENGTH, "upsert: expected vector of length %lld, got %lld", ll,
                                           AS_LIST(lst)[i]->len));
            }

            if (keys == 1) {
                k1 = at_idx(AS_LIST(obj)[1], 0);
                k2 = at_idx(lst, 0);
                m = ops_count(k2);
            } else {
                k1 = ray_take(x[1], AS_LIST(obj)[1]);
                k2 = ray_take(x[1], lst);
                m = ops_count(AS_LIST(k2)[0]);
            }

            idx = index_upsert_obj(k2, k1, x[1]->i64);

            drop_obj(k1);
            drop_obj(k2);

            if (IS_ERR(idx))
                UPSERT_ERROR(idx);

            // Check all the elements of the list
            for (i = 0; i < l; i++) {
                if (!__suitable_types(AS_LIST(AS_LIST(obj)[1])[i], AS_LIST(lst)[i])) {
                    drop_obj(idx);
                    UPSERT_ERROR(ray_error(ERR_TYPE, "upsert: expected '%s' as %lldth element, got '%s'",
                                           type_name(AS_LIST(AS_LIST(obj)[1])[i]->type), i,
                                           type_name(AS_LIST(lst)[i]->type)));
                }

                if (AS_LIST(lst)[i]->len != m) {
                    drop_obj(idx);
                    UPSERT_ERROR(ray_error(
                        ERR_LENGTH, "upsert: expected list of length %lld, as %lldth element in a values, got %lld",
                        AS_LIST(AS_LIST(obj)[1])[i]->len, i, n));
                }
            }

            rows = AS_I64(idx);

            // Process each column
            for (i = 0; i < p; i++) {
                col = cow_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                if (col != AS_LIST(AS_LIST(obj)[1])[i]) {
                    drop_obj(AS_LIST(AS_LIST(obj)[1])[i]);
                    AS_LIST(AS_LIST(obj)[1])[i] = col;
                }

                for (j = 0; j < m; j++) {
                    row = rows[j];

                    // Insert record
                    if (row == NULL_I64) {
                        v = (i < l) ? at_idx(AS_LIST(lst)[i], j) : null(AS_LIST(AS_LIST(obj)[1])[i]->type);
                        push_obj(AS_LIST(AS_LIST(obj)[1]) + i, v);
                    }
                    // Update record (we can skip the keys since they are matches, and only update provided columns)
                    else if (i >= keys && i < l) {
                        v = at_idx(AS_LIST(lst)[i], j);
                        set_idx(AS_LIST(AS_LIST(obj)[1]) + i, row, v);
                    }
                }
            }

            drop_obj(idx);

            if (lst_allocated)
                drop_obj(lst);
            return __commit(x[0], obj, val);

        default:
            UPSERT_ERROR(ray_error(ERR_TYPE, "upsert: unsupported type '%s' in values (forgot to use list?)",
                                   type_name(lst->type)));
    }
}
#undef UPSERT_ERROR

obj_p __update_table(obj_p tab, obj_p keys, obj_p vals, obj_p filters, obj_p groupby) {
    i64_t i, l, m, n;
    i64_t j, *ids;
    obj_p prm, obj, *val = NULL, gids, v, col, index, res, original;

    // No filters nor groupings
    if (filters == NULL_OBJ && groupby == NULL_OBJ) {
        prm = vn_list(4, tab, env_get_internal_function_by_id(SYMBOL_SET), keys, vals);
        obj = ray_alter(AS_LIST(prm), prm->len);
        drop_obj(prm);

        return obj;
    }
    // Groupings
    else if (groupby != NULL_OBJ) {
        obj = __fetch(tab, &val, &original);
        if (IS_ERR(obj)) {
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(vals);
            drop_obj(filters);
            drop_obj(groupby);

            return obj;
        }

        l = keys->len;

        index = index_group(groupby, filters);
        gids = aggr_row(groupby, index);
        drop_obj(index);
        drop_obj(groupby);
        n = gids->len;

        // Check each column
        for (i = 0; i < l; i++) {
            j = find_raw(AS_LIST(obj)[0], AS_I64(keys) + i);

            // Add new column
            if (j == NULL_I64) {
                push_raw(AS_LIST(obj), AS_SYMBOL(keys) + i);
                push_obj(AS_LIST(obj) + 1, nullv(AS_LIST(vals)[i]->type, ops_count(obj)));
            }
            // Check existing column
            else {
                for (m = 0; m < n; m++) {
                    v = at_idx(AS_LIST(vals)[i], m);
                    if (!__suitable_types(AS_LIST(AS_LIST(obj)[1])[j], v)) {
                        res = ray_error(ERR_TYPE, "update: expected '%s as %lldth element, got '%s",
                                        type_name(AS_LIST(AS_LIST(obj)[1])[j]->type), j, type_name(v->type));
                        drop_obj(v);
                        drop_obj(tab);
                        drop_obj(keys);
                        drop_obj(vals);
                        drop_obj(filters);
                        drop_obj(gids);
                        UNCOW_OBJ(obj, val, original, res);
                    }

                    if (!__suitable_lengths(AS_LIST(AS_LIST(obj)[1])[j], obj)) {
                        res = ray_error(
                            ERR_LENGTH,
                            "update: expected '%s of length %lld, as %lldth element in a values, got '%s of %lld",
                            type_name(AS_LIST(AS_LIST(obj)[1])[j]->type), AS_LIST(AS_LIST(obj)[1])[j]->len, j,
                            type_name(v->type), ops_count(v));
                        drop_obj(v);
                        drop_obj(tab);
                        drop_obj(keys);
                        drop_obj(vals);
                        drop_obj(filters);
                        drop_obj(gids);
                        UNCOW_OBJ(obj, val, original, res);
                    }

                    drop_obj(v);
                }
            }
        }

        // Cow each column
        for (i = 0; i < l; i++) {
            j = find_raw(AS_LIST(obj)[0], AS_I64(keys) + i);

            col = cow_obj(AS_LIST(AS_LIST(obj)[1])[j]);
            if (col != AS_LIST(AS_LIST(obj)[1])[j]) {
                drop_obj(AS_LIST(AS_LIST(obj)[1])[j]);
                AS_LIST(AS_LIST(obj)[1])[j] = col;
            }
        }

        // Update by groups
        for (i = 0; i < l; i++) {
            j = find_raw(AS_LIST(obj)[0], AS_I64(keys) + i);

            for (m = 0; m < n; m++) {
                ids = AS_I64(AS_LIST(gids)[m]);
                set_ids(AS_LIST(AS_LIST(obj)[1]) + j, ids, AS_LIST(gids)[m]->len, at_idx(AS_LIST(vals)[i], m));
            }
        }

        drop_obj(keys);
        drop_obj(vals);
        drop_obj(filters);
        drop_obj(gids);

        res = __commit(tab, obj, val);
        drop_obj(tab);

        return res;
    }
    // Filters
    else {
        obj = __fetch(tab, &val, &original);
        if (IS_ERR(obj)) {
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(vals);
            drop_obj(filters);

            return obj;
        }

        l = keys->len;

        // Check each column
        for (i = 0; i < l; i++) {
            j = find_raw(AS_LIST(obj)[0], AS_I64(keys) + i);

            // Add new column
            if (j == NULL_I64) {
                push_raw(AS_LIST(obj), AS_SYMBOL(keys) + i);
                push_obj(AS_LIST(obj) + 1, nullv(AS_LIST(vals)[i]->type, ops_count(obj)));
            }
            // Check existing column
            else {
                if (!__suitable_types(AS_LIST(AS_LIST(obj)[1])[j], AS_LIST(vals)[i])) {
                    res = ray_error(ERR_TYPE, "update: expected '%s as %lldth element, got '%s",
                                    type_name(AS_LIST(AS_LIST(obj)[1])[j]->type), j, type_name(AS_LIST(vals)[i]->type));
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(vals);
                    drop_obj(filters);
                    UNCOW_OBJ(obj, val, original, res);
                }

                // When using filters, allow:
                // 1. Scalars (atoms) - will be applied to all filtered rows
                // 2. Vectors matching filter length - one value per filtered row
                // 3. Vectors matching full table length - will use first filter->len elements
                i64_t vals_len = ops_count(AS_LIST(vals)[i]);
                if (!IS_ATOM(AS_LIST(vals)[i]) && vals_len != filters->len &&
                    vals_len != AS_LIST(AS_LIST(obj)[1])[j]->len) {
                    res = ray_error(ERR_LENGTH,
                                    "update: expected '%s of length %lld (filter length) or %lld (table length), as "
                                    "%lldth element, got '%s of %lld",
                                    type_name(AS_LIST(AS_LIST(obj)[1])[j]->type), filters->len,
                                    AS_LIST(AS_LIST(obj)[1])[j]->len, j, type_name(AS_LIST(vals)[i]->type), vals_len);
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(vals);
                    drop_obj(filters);
                    UNCOW_OBJ(obj, val, original, res);
                }
            }
        }

        // Cow each column
        for (i = 0; i < l; i++) {
            j = find_raw(AS_LIST(obj)[0], AS_I64(keys) + i);

            col = cow_obj(AS_LIST(AS_LIST(obj)[1])[j]);
            if (col != AS_LIST(AS_LIST(obj)[1])[j]) {
                drop_obj(AS_LIST(AS_LIST(obj)[1])[j]);
                AS_LIST(AS_LIST(obj)[1])[j] = col;
            }
        }

        ids = AS_I64(filters);

        for (i = 0; i < l; i++) {
            j = find_raw(AS_LIST(obj)[0], AS_I64(keys) + i);
            set_ids(AS_LIST(AS_LIST(obj)[1]) + j, ids, filters->len, at_idx(vals, i));
        }

        drop_obj(keys);
        drop_obj(vals);
        drop_obj(filters);

        res = __commit(tab, obj, val);
        drop_obj(tab);

        return res;
    }
}

obj_p ray_update(obj_p obj) {
    i64_t i, keyslen, tablen;
    obj_p tabsym, keys = NULL_OBJ, vals = NULL_OBJ, filters = NULL_OBJ, bins = NULL_OBJ, groupby = NULL_OBJ, tab, sym,
                  prm, val;

    if (obj->type != TYPE_DICT)
        THROW(ERR_LENGTH, "'update' takes dict of params");

    if (AS_LIST(obj)[0]->type != TYPE_SYMBOL)
        THROW(ERR_LENGTH, "'update' takes dict with symbol keys");

    // Retrive a table
    tabsym = at_sym(obj, "from", 4);

    if (is_null(tabsym))
        THROW(ERR_LENGTH, "'update' expects 'from' param");

    tab = eval(tabsym);
    if (IS_ERR(tab)) {
        drop_obj(tabsym);
        return tab;
    }

    if (tab->type == -TYPE_SYMBOL) {
        val = eval(tab);
        drop_obj(tab);
        tab = val;
    } else {
        drop_obj(tabsym);
        tabsym = clone_obj(tab);
    }

    if (tab->type != TYPE_TABLE) {
        drop_obj(tabsym);
        drop_obj(tab);
        THROW(ERR_TYPE, "'update' from: expects table");
    }

    keys = ray_except(AS_LIST(obj)[0], runtime_get()->env.keywords);
    keyslen = keys->len;

    if (keyslen == 0) {
        drop_obj(tabsym);
        drop_obj(keys);
        drop_obj(tab);
        THROW(ERR_LENGTH, "'update' expects at least one field to update");
    }

    // Mount table columns to a local env
    tablen = AS_LIST(tab)[0]->len;
    mount_env(tab);

    // Apply filters
    prm = at_sym(obj, "where", 5);
    if (prm != NULL_OBJ) {
        val = eval(prm);
        drop_obj(prm);
        if (IS_ERR(val)) {
            drop_obj(tabsym);
            drop_obj(tab);
            return val;
        }

        filters = ray_where(val);
        drop_obj(val);
        if (IS_ERR(filters)) {
            drop_obj(tabsym);
            drop_obj(tab);
            return filters;
        }
    }

    // Apply groupping
    prm = at_sym(obj, "by", 2);
    if (prm != NULL_OBJ) {
        groupby = eval(prm);
        drop_obj(prm);

        unmount_env(tablen);

        if (IS_ERR(groupby)) {
            drop_obj(tabsym);
            drop_obj(tab);
            return groupby;
        }

        bins = index_group(groupby, filters);
        prm = group_map(tab, bins);
        drop_obj(bins);

        if (IS_ERR(prm)) {
            drop_obj(tabsym);
            drop_obj(tab);
            drop_obj(filters);
            drop_obj(groupby);
            return prm;
        }

        mount_env(prm);
        drop_obj(prm);
    } else if (filters != NULL_OBJ) {
        // Unmount table columns from a local env
        unmount_env(tablen);
        // Create filtermaps over table
        val = remap_filter(tab, filters);
        mount_env(val);
        drop_obj(val);
    }

    // Apply mappings
    vals = LIST(keyslen);
    for (i = 0; i < keyslen; i++) {
        sym = at_idx(keys, i);
        prm = at_obj(obj, sym);
        drop_obj(sym);
        val = eval(prm);
        drop_obj(prm);

        if (IS_ERR(val)) {
            vals->len = i;
            drop_obj(tabsym);
            drop_obj(vals);
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(groupby);

            return val;
        }

        // Materialize fields
        if (val->type == TYPE_MAPGROUP) {
            prm = aggr_collect(AS_LIST(val)[0], AS_LIST(val)[1]);
            drop_obj(val);
            val = prm;
        } else if (val->type == TYPE_MAPFILTER) {
            prm = filter_collect(AS_LIST(val)[0], AS_LIST(val)[1]);
            drop_obj(val);
            val = prm;
        } else if (val->type == TYPE_ENUM) {
            prm = ray_value(val);
            drop_obj(val);
            val = prm;
        }

        if (IS_ERR(val)) {
            vals->len = i;
            drop_obj(tabsym);
            drop_obj(vals);
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(groupby);

            return val;
        }

        AS_LIST(vals)[i] = val;
    }

    unmount_env(tablen);
    drop_obj(tab);

    // This one will take care of dropping all the arguments
    return __update_table(tabsym, keys, vals, filters, groupby);
}
