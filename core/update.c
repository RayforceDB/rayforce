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
#include "heap.h"
#include "util.h"
#include "runtime.h"
#include "error.h"
#include "eval.h"
#include "filter.h"
#include "binary.h"
#include "unary.h"
#include "vary.h"
#include "iter.h"
#include "index.h"
#include "items.h"
#include "group.h"
#include "filter.h"
#include "query.h"

#define uncow_obj(o, v, r)            \
    {                                 \
        if ((v == NULL) || (*v != o)) \
            drop_obj(o);              \
        return r;                     \
    };

obj_p __fetch(obj_p obj, obj_p **val)
{
    if (obj->type == -TYPE_SYMBOL)
    {
        *val = deref(obj);
        if (*val == NULL)
            throw(ERR_NOT_FOUND, "fetch: symbol not found");

        obj = cow_obj(**val);
    }
    else
        obj = cow_obj(obj);

    return obj;
}

b8_t __suitable_types(obj_p x, obj_p y)
{
    i8_t yt;

    if (y->type < 0)
        yt = -y->type;
    else
        yt = y->type;

    if ((x->type != TYPE_LIST) &&
        (x->type != TYPE_ANYMAP) &&
        (x->type != yt) &&
        (x->type != TYPE_ENUM && yt != TYPE_SYMBOL))
    {
        return B8_FALSE;
    }

    return B8_TRUE;
}

b8_t __suitable_lengths(obj_p x, obj_p y)
{
    if (x->type != TYPE_LIST && (is_vector(y) && (ops_count(y) != ops_count(x))))
        return B8_FALSE;

    return B8_TRUE;
}

obj_p __alter(obj_p *obj, obj_p *x, u64_t n)
{
    obj_p v, idx, res;

    // special case for set
    if (x[1]->i64 == (i64_t)ray_set || x[1]->i64 == (i64_t)ray_let)
    {
        if (n != 4)
            throw(ERR_LENGTH, "alter: set expected a value");

        return set_obj(obj, x[2], clone_obj(x[3]));
    }

    // retrieve the object via indices
    v = at_obj(*obj, x[2]);

    if (is_error(v))
        return v;

    idx = x[2];
    x[2] = v;
    res = ray_apply(x + 1, n - 1);
    x[2] = idx;
    drop_obj(v);

    if (is_error(res))
        return res;

    return set_obj(obj, idx, res);
}

obj_p __commit(obj_p src, obj_p obj, obj_p *val)
{
    if (src->type == -TYPE_SYMBOL)
    {
        if ((val != NULL) && (*val != obj))
        {
            drop_obj(*val);
            *val = obj;
        }

        return clone_obj(src);
    }

    return obj;
}

obj_p ray_alter(obj_p *x, u64_t n)
{
    obj_p *val = NULL, obj, res;

    if (n < 3)
        throw(ERR_LENGTH, "alter: expected at least 3 arguments");

    if (x[1]->type < TYPE_LAMBDA || x[1]->type > TYPE_VARY)
        throw(ERR_TYPE, "alter: expected function as 2nd argument");

    obj = __fetch(x[0], &val);

    if (is_error(obj))
        return obj;

    res = __alter(&obj, x, n);
    if (is_error(res))
    {
        uncow_obj(obj, val, res);
        return res;
    }

    return __commit(x[0], obj, val);
}

obj_p __modify(obj_p *obj, obj_p *x, u64_t n)
{
    obj_p v, idx, res;

    // special case for set
    if (x[1]->i64 == (i64_t)ray_set || x[1]->i64 == (i64_t)ray_let)
    {
        if (n != 4)
            throw(ERR_LENGTH, "alter: set expected a value");

        return set_obj(obj, x[2], clone_obj(x[3]));
    }

    // retrieve the object via indices
    v = at_obj(*obj, x[2]);

    if (is_error(v))
        return v;

    idx = x[2];
    x[2] = v;
    res = ray_apply(x + 1, n - 1);
    x[2] = idx;
    drop_obj(v);

    if (is_error(res))
        return res;

    return set_obj(obj, idx, res);
}

obj_p ray_modify(obj_p *x, u64_t n)
{
    obj_p *val = NULL, obj, res;

    if (n < 3)
        throw(ERR_LENGTH, "modify: expected at least 3 arguments, got %lld", n);

    if (x[1]->type < TYPE_LAMBDA || x[1]->type > TYPE_VARY)
        throw(ERR_TYPE, "modify: expected function as 2nd argument, got '%s'", type_name(x[1]->type));

    obj = __fetch(x[0], &val);

    if (is_error(obj))
        return obj;

    res = __alter(&obj, x, n);
    if (is_error(res))
        uncow_obj(obj, val, res);

    return obj;
}

/*
 * inserts for tables
 */
obj_p ray_insert(obj_p *x, u64_t n)
{
    u64_t i, m, l;
    obj_p lst, col, *val = NULL, obj, res;
    b8_t need_drop;

    if (n != 2)
        throw(ERR_LENGTH, "insert: expected 2 arguments, got %lld", n);

    obj = __fetch(x[0], &val);

    if (is_error(obj))
        return obj;

    if (obj->type != TYPE_TABLE)
    {
        res = error(ERR_TYPE, "insert: expected 'Table as 1st argument, got '%s", type_name(obj->type));
        uncow_obj(obj, val, res);
    }

    // Check integrity of the table with the new object
    lst = x[1];
insert:
    switch (lst->type)
    {
    case TYPE_LIST:
        l = lst->len;
        if (l != as_list(obj)[0]->len)
        {
            res = error(ERR_LENGTH, "insert: expected list of length %lld, got %lld", as_list(obj)[0]->len, l);
            uncow_obj(obj, val, res);
        }

        // There is one record to be inserted
        if (is_atom(as_list(lst)[0]))
        {
            // Check all the elements of the list
            for (i = 0; i < l; i++)
            {
                if (!__suitable_types(as_list(as_list(obj)[1])[i], as_list(lst)[i]))
                {
                    res = error(ERR_TYPE, "insert: expected '%s' as %lldth element in a values list, got '%s'", type_name(-as_list(as_list(obj)[1])[i]->type), i, type_name(as_list(lst)[i]->type));
                    uncow_obj(obj, val, res);
                }
            }

            // Insert the record now
            for (i = 0; i < l; i++)
            {
                col = cow_obj(as_list(as_list(obj)[1])[i]);
                need_drop = (col != as_list(as_list(obj)[1])[i]);
                push_obj(&col, clone_obj(as_list(lst)[i]));
                if (need_drop)
                    drop_obj(as_list(as_list(obj)[1])[i]);
                as_list(as_list(obj)[1])[i] = col;
            }
        }
        else
        {
            // There are multiple records to be inserted
            m = as_list(lst)[0]->len;
            if (m == 0)
            {
                res = error(ERR_LENGTH, "insert: expected non-empty list of records");
                uncow_obj(obj, val, res);
            }

            // Check all the elements of the list
            for (i = 0; i < l; i++)
            {
                if (!__suitable_types(as_list(as_list(obj)[1])[i], as_list(lst)[i]))
                {
                    res = error(ERR_TYPE, "insert: expected '%s' as %lldth element, got '%s'", type_name(as_list(as_list(obj)[1])[i]->type), i, type_name(as_list(lst)[i]->type));
                    uncow_obj(obj, val, res);
                }

                if (as_list(lst)[i]->len != m)
                {
                    res = error(ERR_LENGTH, "insert: expected list of length %lld, as %lldth element in a values, got %lld", as_list(as_list(obj)[1])[i]->len, i, n);
                    uncow_obj(obj, val, res);
                }
            }

            // Insert all the records now
            for (i = 0; i < l; i++)
            {
                col = cow_obj(as_list(as_list(obj)[1])[i]);
                need_drop = (col != as_list(as_list(obj)[1])[i]);
                append_list(&col, as_list(lst)[i]);
                if (need_drop)
                    drop_obj(as_list(as_list(obj)[1])[i]);
                as_list(as_list(obj)[1])[i] = col;
            }
        }

        break;

    case TYPE_DICT:
        if (as_list(lst)[0]->type != TYPE_SYMBOL)
        {
            res = error(ERR_TYPE, "insert: expected 'Symbol as 1st element in a dictionary, got '%s'", type_name(as_list(lst)[0]->type));
            uncow_obj(obj, val, res);
        }
        // Fall through
    case TYPE_TABLE:
        // Check columns
        l = as_list(lst)[0]->len;
        if (l != as_list(obj)[0]->len)
        {
            res = error(ERR_LENGTH, "insert: expected 'Table with the same number of columns");
            uncow_obj(obj, val, res);
        }

        for (i = 0; i < l; i++)
        {
            if (as_symbol(as_list(lst)[0])[i] != as_symbol(as_list(obj)[0])[i])
            {
                res = error(ERR_TYPE, "insert: expected 'Table with the same columns");
                uncow_obj(obj, val, res);
            }
        }

        lst = as_list(lst)[1];
        goto insert;

    default:
        res = error(ERR_TYPE, "insert: unsupported type '%s' as 2nd argument", type_name(lst->type));
        uncow_obj(obj, val, res);
    }

    return __commit(x[0], obj, val);
}

/*
 * update/inserts for tables
 */
obj_p ray_upsert(obj_p *x, u64_t n)
{
    u64_t i, j, m, p, l, keys;
    i64_t row, *rows;
    obj_p obj, k1, k2, idx, col, lst, *val = NULL, v;
    b8_t single_rec;

    if (n != 3)
        throw(ERR_LENGTH, "upsert: expected 3 arguments, got %lld", n);

    if (x[1]->type != -TYPE_I64)
        throw(ERR_TYPE, "upsert: expected 'I64 as 2nd argument, got '%s'", type_name(x[1]->type));

    keys = x[1]->i64;
    if (keys < 1)
        throw(ERR_LENGTH, "upsert: expected positive number of keys > 0, got %lld", keys);

    obj = __fetch(x[0], &val);
    if (is_error(obj))
        return obj;

    if (obj->type != TYPE_TABLE)
    {
        drop_obj(obj);
        return error(ERR_TYPE, "upsert: expected 'Table as 1st argument, got '%s'", type_name(obj->type));
    }

    p = as_list(obj)[0]->len;

    lst = x[2];

upsert:
    switch (lst->type)
    {
    case TYPE_LIST:
        l = ops_count(lst);

        single_rec = is_atom(as_list(lst)[0]);

        if (l > p)
        {
            drop_obj(obj);
            return error(ERR_LENGTH, "upsert: list length %lld is greater than %lld", l, p);
        }

        if (keys == 1)
        {
            k1 = at_idx(as_list(obj)[1], 0);
            k2 = at_idx(lst, 0);
            m = ops_count(k2);
        }
        else
        {
            k1 = ray_take(x[1], as_list(obj)[1]);
            k2 = ray_take(x[1], lst);
            m = ops_count(as_list(k2)[0]);
        }

        idx = index_join_obj(k2, k1, x[1]->i64);

        drop_obj(k1);
        drop_obj(k2);

        if (is_error(idx))
        {
            drop_obj(obj);
            return idx;
        }

        // Check integrity of the table with the new object
        if (single_rec)
        {
            // Check all the elements of the list
            for (i = 0; i < l; i++)
            {
                if (!__suitable_types(as_list(as_list(obj)[1])[i], as_list(lst)[i]))
                {
                    drop_obj(idx);
                    drop_obj(obj);
                    return error(ERR_TYPE, "upsert: expected '%s' as %lldth element, got '%s'", type_name(-as_list(as_list(obj)[1])[i]->type), i, type_name(as_list(lst)[i]->type));
                }
            }
        }
        else
        {
            // Check all the elements of the list
            for (i = 0; i < l; i++)
            {
                if (!__suitable_types(as_list(as_list(obj)[1])[i], as_list(lst)[i]))
                {
                    drop_obj(idx);
                    drop_obj(obj);
                    return error(ERR_TYPE, "upsert: expected '%s' as %lldth element, got '%s'", type_name(as_list(as_list(obj)[1])[i]->type), i, type_name(as_list(lst)[i]->type));
                }

                if (as_list(lst)[i]->len != m)
                {
                    drop_obj(idx);
                    drop_obj(obj);
                    return error(ERR_LENGTH, "upsert: expected list of length %lld, as %lldth element in a values, got %lld", as_list(as_list(obj)[1])[i]->len, i, n);
                }
            }
        }

        rows = as_i64(idx);

        // Process each column
        for (i = 0; i < p; i++)
        {
            col = cow_obj(as_list(as_list(obj)[1])[i]);
            if (col != as_list(as_list(obj)[1])[i])
            {
                drop_obj(as_list(as_list(obj)[1])[i]);
                as_list(as_list(obj)[1])[i] = col;
            }

            for (j = 0; j < m; j++)
            {
                row = rows[j];

                // Insert record
                if (row == NULL_I64)
                {
                    v = (i < l) ? (single_rec ? clone_obj(as_list(lst)[i]) : at_idx(as_list(lst)[i], j)) : null(as_list(as_list(obj)[1])[i]->type);
                    push_obj(as_list(as_list(obj)[1]) + i, v);
                }
                // Update record (we can skip the keys since they are matches)
                else if (i >= keys && i < l)
                {
                    v = single_rec ? clone_obj(as_list(lst)[i]) : at_idx(as_list(lst)[i], j);
                    set_idx(as_list(as_list(obj)[1]) + i, row, v);
                }
            }
        }

        drop_obj(idx);

        return __commit(x[0], obj, val);
    case TYPE_DICT:
        if (as_list(lst)[0]->type != TYPE_SYMBOL)
        {
            drop_obj(obj);
            return error(ERR_TYPE, "upsert: expected 'Symbol as 1st element in a dictionary, got '%s'", type_name(as_list(lst)[0]->type));
        }
        // Fall through
    case TYPE_TABLE:
        // Check columns
        l = as_list(lst)[0]->len;

        if (l > p)
        {
            drop_obj(obj);
            return error(ERR_LENGTH, "upsert: 'Table with inconsistent columns");
        }

        for (i = 0; i < l; i++)
        {
            if (as_symbol(as_list(lst)[0])[i] != as_symbol(as_list(obj)[0])[i])
            {
                drop_obj(obj);
                return error(ERR_TYPE, "upsert: expected 'Table with inconsistent columns: '%s != '%s",
                             strof_sym(as_symbol(as_list(lst)[0])[i]), strof_sym(as_symbol(as_list(obj)[0])[i]));
            }
        }

        lst = as_list(lst)[1];
        goto upsert;

    default:
        drop_obj(obj);
        return error(ERR_TYPE, "upsert: unsupported type '%s' as 2nd argument", type_name(lst->type));
    }
}

obj_p __update_table(obj_p tab, obj_p keys, obj_p vals, obj_p filters, obj_p groupby)
{
    u64_t i, j, l, m, n;
    obj_p prm, obj, *val = NULL, gids, v, col, res;
    i64_t *ids;

    // No filters nor groupings
    if (filters == NULL_OBJ && groupby == NULL_OBJ)
    {
        prm = vn_list(4, tab, env_get_internal_function_by_id(KW_SET), keys, vals);
        obj = ray_alter(as_list(prm), prm->len);
        drop_obj(prm);

        return obj;
    }
    // Groupings
    else if (groupby != NULL_OBJ)
    {
        obj = __fetch(tab, &val);
        if (is_error(obj))
        {
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(vals);
            drop_obj(filters);
            drop_obj(groupby);

            return obj;
        }

        l = keys->len;

        ids = (filters != NULL_OBJ) ? as_i64(filters) : NULL;
        gids = group_ids(groupby, ids);
        drop_obj(groupby);
        n = gids->len;

        // Check each column
        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], as_i64(keys) + i);

            // Add new column
            if (j == as_list(obj)[0]->len)
            {
                push_raw(as_list(obj), as_symbol(keys) + i);
                push_obj(as_list(obj) + 1, nullv(as_list(vals)[i]->type, ops_count(obj)));
            }
            // Check existing column
            else
            {
                for (m = 0; m < n; m++)
                {
                    v = at_idx(as_list(vals)[i], m);
                    if (!__suitable_types(as_list(as_list(obj)[1])[j], v))
                    {
                        res = error(ERR_TYPE, "update: expected '%s as %lldth element, got '%s",
                                    type_name(as_list(as_list(obj)[1])[j]->type), j, type_name(v->type));
                        drop_obj(v);
                        drop_obj(tab);
                        drop_obj(keys);
                        drop_obj(vals);
                        drop_obj(filters);
                        drop_obj(gids);
                        uncow_obj(obj, val, res);
                    }

                    if (!__suitable_lengths(as_list(as_list(obj)[1])[j], obj))
                    {
                        res = error(ERR_LENGTH, "update: expected '%s of length %lld, as %lldth element in a values, got '%s of %lld",
                                    type_name(as_list(as_list(obj)[1])[j]->type), as_list(as_list(obj)[1])[j]->len,
                                    j, type_name(v->type), ops_count(v));
                        drop_obj(v);
                        drop_obj(tab);
                        drop_obj(keys);
                        drop_obj(vals);
                        drop_obj(filters);
                        drop_obj(gids);
                        uncow_obj(obj, val, res);
                    }

                    drop_obj(v);
                }
            }
        }

        // Cow each column
        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], as_i64(keys) + i);

            col = cow_obj(as_list(as_list(obj)[1])[j]);
            if (col != as_list(as_list(obj)[1])[j])
            {
                drop_obj(as_list(as_list(obj)[1])[j]);
                as_list(as_list(obj)[1])[j] = col;
            }
        }

        // Update by groups
        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], as_i64(keys) + i);

            for (m = 0; m < n; m++)
            {
                ids = as_i64(as_list(gids)[m]);
                set_ids(as_list(as_list(obj)[1]) + j, ids, as_list(gids)[m]->len, at_idx(as_list(vals)[i], m));
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
    else
    {
        obj = __fetch(tab, &val);
        if (is_error(obj))
        {
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(vals);
            drop_obj(filters);

            return obj;
        }

        l = keys->len;

        // Check each column
        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], as_i64(keys) + i);

            // Add new column
            if (j == as_list(obj)[0]->len)
            {
                push_raw(as_list(obj), as_symbol(keys) + i);
                push_obj(as_list(obj) + 1, nullv(as_list(vals)[i]->type, ops_count(obj)));
            }
            // Check existing column
            else
            {
                if (!__suitable_types(as_list(as_list(obj)[1])[j], as_list(vals)[i]))
                {
                    res = error(ERR_TYPE, "update: expected '%s as %lldth element, got '%s",
                                type_name(as_list(as_list(obj)[1])[j]->type), j, type_name(as_list(vals)[i]->type));
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(vals);
                    drop_obj(filters);
                    uncow_obj(obj, val, res);
                }

                if (!__suitable_lengths(as_list(as_list(obj)[1])[j], as_list(vals)[i]))
                {
                    res = error(ERR_LENGTH, "update: expected '%s of length %lld, as %lldth element in a values, got '%s of %lld",
                                type_name(as_list(as_list(obj)[1])[j]->type), as_list(as_list(obj)[1])[j]->len,
                                j, type_name(as_list(vals)[i]->type), ops_count(as_list(vals)[i]));
                    drop_obj(tab);
                    drop_obj(keys);
                    drop_obj(vals);
                    drop_obj(filters);
                    uncow_obj(obj, val, res);
                }
            }
        }

        // Cow each column
        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], as_i64(keys) + i);

            col = cow_obj(as_list(as_list(obj)[1])[j]);
            if (col != as_list(as_list(obj)[1])[j])
            {
                drop_obj(as_list(as_list(obj)[1])[j]);
                as_list(as_list(obj)[1])[j] = col;
            }
        }

        ids = as_i64(filters);

        for (i = 0; i < l; i++)
        {
            j = find_raw(as_list(obj)[0], as_i64(keys) + i);
            set_ids(as_list(as_list(obj)[1]) + j, ids, filters->len, at_idx(vals, i));
        }

        drop_obj(keys);
        drop_obj(vals);
        drop_obj(filters);

        res = __commit(tab, obj, val);
        drop_obj(tab);

        return res;
    }
}

obj_p ray_update(obj_p obj)
{
    u64_t i, keyslen, tablen;
    obj_p tabsym, keys = NULL_OBJ, vals = NULL_OBJ, filters = NULL_OBJ,
                  bins = NULL_OBJ, groupby = NULL_OBJ, tab, sym, prm, val;

    if (obj->type != TYPE_DICT)
        throw(ERR_LENGTH, "'update' takes dict of params");

    if (as_list(obj)[0]->type != TYPE_SYMBOL)
        throw(ERR_LENGTH, "'update' takes dict with symbol keys");

    // Retrive a table
    tabsym = at_sym(obj, "from");

    if (is_null(tabsym))
        throw(ERR_LENGTH, "'update' expects 'from' param");

    tab = eval(tabsym);
    if (is_error(tab))
    {
        drop_obj(tabsym);
        return tab;
    }

    if (tab->type == -TYPE_SYMBOL)
    {
        val = eval(tab);
        drop_obj(tab);
        tab = val;
    }
    else
    {
        drop_obj(tabsym);
        tabsym = clone_obj(tab);
    }

    if (tab->type != TYPE_TABLE)
    {
        drop_obj(tabsym);
        drop_obj(tab);
        throw(ERR_TYPE, "'update' from: expects table");
    }

    keys = get_fields(obj);
    keyslen = keys->len;

    if (keyslen == 0)
    {
        drop_obj(tabsym);
        drop_obj(keys);
        drop_obj(tab);
        throw(ERR_LENGTH, "'update' expects at least one field to update");
    }

    // Mount table columns to a local env
    tablen = as_list(tab)[0]->len;
    mount_env(tab);

    // Apply filters
    prm = at_sym(obj, "where");
    if (prm != NULL_OBJ)
    {
        val = eval(prm);
        drop_obj(prm);
        if (is_error(val))
        {
            drop_obj(tabsym);
            drop_obj(tab);
            return val;
        }

        filters = ray_where(val);
        drop_obj(val);
        if (is_error(filters))
        {
            drop_obj(tabsym);
            drop_obj(tab);
            return filters;
        }
    }

    // Apply groupping
    prm = at_sym(obj, "by");
    if (prm != NULL_OBJ)
    {
        groupby = eval(prm);
        drop_obj(prm);

        unmount_env(tablen);

        if (is_error(groupby))
        {
            drop_obj(tabsym);
            drop_obj(tab);
            return groupby;
        }

        bins = group_bins(groupby, tab, filters);
        drop_obj(groupby);
        prm = group_map(tab, bins, filters);

        if (is_error(prm))
        {
            drop_obj(tabsym);
            drop_obj(tab);
            drop_obj(filters);
            drop_obj(bins);
            return prm;
        }

        mount_env(prm);
        drop_obj(prm);
    }
    else if (filters != NULL_OBJ)
    {
        // Unmount table columns from a local env
        unmount_env(tablen);
        // Create filtermaps over table
        val = remap_filter(tab, filters);
        mount_env(val);
        drop_obj(val);
    }

    // Apply mappings
    vals = list(keyslen);
    for (i = 0; i < keyslen; i++)
    {
        sym = at_idx(keys, i);
        prm = at_obj(obj, sym);
        drop_obj(sym);
        val = eval(prm);
        drop_obj(prm);

        if (is_error(val))
        {
            vals->len = i;
            drop_obj(tabsym);
            drop_obj(vals);
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(bins);

            return val;
        }

        // Materialize fields
        if (val->type == TYPE_GROUPMAP)
        {
            prm = group_collect(val);
            drop_obj(val);
            val = prm;
        }
        else if (val->type == TYPE_FILTERMAP)
        {
            prm = filter_collect(val);
            drop_obj(val);
            val = prm;
        }
        else if (val->type == TYPE_ENUM)
        {
            prm = ray_value(val);
            drop_obj(val);
            val = prm;
        }

        if (is_error(val))
        {
            vals->len = i;
            drop_obj(tabsym);
            drop_obj(vals);
            drop_obj(tab);
            drop_obj(keys);
            drop_obj(bins);

            return val;
        }

        as_list(vals)[i] = val;
    }

    unmount_env(tablen);
    drop_obj(tab);

    // This one will take care of dropping all the arguments
    return __update_table(tabsym, keys, vals, filters, bins);
}
