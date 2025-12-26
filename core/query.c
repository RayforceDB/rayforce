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

#include "query.h"
#include "env.h"
#include "unary.h"
#include "eval.h"
#include "items.h"
#include "compose.h"
#include "error.h"
#include "aggr.h"
#include "index.h"
#include "group.h"
#include "filter.h"
#include "chrono.h"
#include "runtime.h"
#include "symbols.h"

obj_p remap_filter(obj_p tab, obj_p index) { return filter_map(tab, index); }

obj_p remap_group(obj_p *gvals, obj_p cols, obj_p gkeys, obj_p gcols, query_ctx_p ctx) {
    i64_t i, l;
    obj_p index, v, lst, res;

    switch (gkeys->type) {
        case -TYPE_SYMBOL:
            index = index_group(cols, ctx->filter);
            timeit_tick("build index");

            if (IS_ERR(index))
                return index;

            ctx->group_index = clone_obj(index);

            res = group_map(ctx->table, index);
            v = (gcols == NULL_OBJ) ? aggr_first(cols, index) : aggr_first(gcols, index);
            if (IS_ERR(v)) {
                drop_obj(index);
                drop_obj(res);
                return v;
            }

            *gvals = v;
            drop_obj(index);

            timeit_tick("apply 'first' on group columns");

            return res;
        case TYPE_SYMBOL:
            index = index_group_list(cols, ctx->filter);
            timeit_tick("build compound index");

            if (IS_ERR(index))
                return index;

            res = group_map(ctx->table, index);

            l = cols->len;
            lst = LIST(l);

            for (i = 0; i < l; i++) {
                v = aggr_first(AS_LIST(cols)[i], index);

                if (IS_ERR(v)) {
                    lst->len = i;
                    drop_obj(res);
                    drop_obj(index);
                    return v;
                }

                AS_LIST(lst)[i] = v;
            }

            *gvals = lst;
            drop_obj(index);

            timeit_tick("apply 'first' on group columns");

            return res;
        default:
            return err_type(0, 0, 0);
    }
}

obj_p get_gkeys(obj_p cols, obj_p obj) {
    i64_t i, l;
    obj_p x;

    switch (obj->type) {
        case -TYPE_SYMBOL:
            l = cols->len;
            for (i = 0; i < l; i++)
                if (AS_I64(cols)[i] == obj->i64)
                    return symboli64(obj->i64);
            return NULL_OBJ;
        case TYPE_LIST:
            l = obj->len;
            for (i = 0; i < l; i++) {
                x = get_gkeys(cols, AS_LIST(obj)[i]);
                if (x != NULL_OBJ)
                    return x;
            }
            return NULL_OBJ;
        case TYPE_DICT:
            x = AS_LIST(obj)[0];
            if (x->type != TYPE_SYMBOL)
                return err_type(0, 0, 0);

            if (x->len == 1)
                return at_idx(AS_LIST(obj)[0], 0);

            return clone_obj(AS_LIST(obj)[0]);

        default:
            return NULL_OBJ;
    }
}

obj_p get_gvals(obj_p obj) {
    i64_t i, l;
    obj_p vals, v, r, res;

    switch (obj->type) {
        case TYPE_DICT:
            vals = AS_LIST(obj)[1];
            l = vals->len;

            if (l == 0)
                return NULL_OBJ;

            if (l == 1) {
                v = at_idx(vals, 0);
                res = eval(v);
                drop_obj(v);
                return res;
            }

            res = LIST(l);
            for (i = 0; i < l; i++) {
                v = at_idx(vals, i);
                r = eval(v);
                drop_obj(v);

                if (IS_ERR(r)) {
                    res->len = i;
                    drop_obj(res);
                    return r;
                }

                AS_LIST(res)
                [i] = r;
            }

            return res;
        default:
            return eval(obj);
    }
}

nil_t query_ctx_init(query_ctx_p ctx) {
    vm_p vm = VM;
    ctx->tablen = 0;
    ctx->table = NULL_OBJ;
    ctx->take = NULL_OBJ;
    ctx->filter = NULL_OBJ;
    ctx->group_fields = NULL_OBJ;
    ctx->group_values = NULL_OBJ;
    ctx->query_fields = NULL_OBJ;
    ctx->query_values = NULL_OBJ;
    ctx->group_index = NULL_OBJ;
    ctx->parent = vm->query_ctx;
    vm->query_ctx = ctx;
}

nil_t query_ctx_destroy(query_ctx_p ctx) {
    VM->query_ctx = ctx->parent;

    drop_obj(ctx->table);
    drop_obj(ctx->take);
    drop_obj(ctx->filter);
    drop_obj(ctx->group_fields);
    drop_obj(ctx->group_values);
    drop_obj(ctx->query_fields);
    drop_obj(ctx->query_values);
    drop_obj(ctx->group_index);
}

obj_p select_fetch_table(obj_p obj, query_ctx_p ctx) {
    obj_p prm, val;

    prm = at_sym(obj, "from", 4);

    if (is_null(prm))
        return err_value(symbols_intern("from", 4));

    val = eval(prm);
    drop_obj(prm);

    if (IS_ERR(val))
        return val;

    if (val->type != TYPE_TABLE) {
        i8_t actual_type = val->type;
        drop_obj(val);
        return err_type(TYPE_TABLE, actual_type, symbols_intern("from", 4));
    }

    ctx->tablen = AS_LIST(val)[0]->len;
    ctx->table = val;

    prm = at_sym(obj, "take", 4);

    if (!is_null(prm)) {
        val = eval(prm);
        drop_obj(prm);

        if (IS_ERR(val))
            return val;

        ctx->take = val;
    }

    timeit_tick("fetch table");

    return NULL_OBJ;
}

obj_p select_apply_filters(obj_p obj, query_ctx_p ctx) {
    obj_p prm, val, fil;

    timeit_span_start("filters");

    prm = at_sym(obj, "where", 5);
    if (prm != NULL_OBJ) {
        val = eval(prm);
        timeit_tick("eval filters");
        drop_obj(prm);

        if (IS_ERR(val))
            return val;

        fil = ray_where(val);
        timeit_tick("find indices");
        drop_obj(val);

        if (IS_ERR(fil))
            return fil;

        ctx->filter = fil;
    }

    timeit_span_end("filters");

    return NULL_OBJ;
}

obj_p select_apply_groupings(obj_p obj, query_ctx_p ctx) {
    obj_p prm, val, gkeys = NULL_OBJ, gvals = NULL_OBJ, groupby = NULL_OBJ, gcol = NULL_OBJ;

    prm = at_sym(obj, "by", 2);
    if (prm != NULL_OBJ) {
        timeit_span_start("group");

        gkeys = get_gkeys(AS_LIST(ctx->table)[0], prm);
        groupby = get_gvals(prm);

        if (gkeys == NULL_OBJ)
            gkeys = symbol("By", 2);
        else if (prm->type != TYPE_DICT)
            gvals = eval(gkeys);

        drop_obj(prm);

        if (IS_ERR(groupby)) {
            drop_obj(gkeys);
            drop_obj(gvals);
            timeit_span_end("group");
            return groupby;
        }

        timeit_tick("get keys");

        prm = remap_group(&gcol, groupby, gkeys, gvals, ctx);

        drop_obj(gvals);
        drop_obj(groupby);

        if (IS_ERR(prm)) {
            drop_obj(gkeys);
            timeit_span_end("group");
            return prm;
        }

        // Replace table with remapped table for column resolution
        drop_obj(ctx->table);
        ctx->table = prm;

        if (IS_ERR(gcol)) {
            drop_obj(gkeys);
            timeit_span_end("group");
            return gcol;
        }

        ctx->group_fields = gkeys;
        ctx->group_values = gcol;

        timeit_span_end("group");
    } else if (ctx->filter != NULL_OBJ) {
        // Remap filtered table for column resolution
        val = remap_filter(ctx->table, ctx->filter);

        if (IS_ERR(val))
            return val;

        // Replace table with filtered table
        drop_obj(ctx->table);
        ctx->table = val;
    }

    return NULL_OBJ;
}

obj_p select_apply_mappings(obj_p obj, query_ctx_p ctx) {
    i64_t i, l;
    obj_p prm, sym, val, keys, res;

    // Find all mappings (non-keyword fields)
    keys = ray_except(AS_LIST(obj)[0], runtime_get()->env.keywords);
    l = keys->len;

    // Mapppings specified
    if (l) {
        res = LIST(l);

        for (i = 0; i < l; i++) {
            sym = at_idx(keys, i);
            prm = at_obj(obj, sym);
            drop_obj(sym);
            val = eval(prm);
            drop_obj(prm);

            if (IS_ERR(val)) {
                res->len = i;
                drop_obj(res);
                drop_obj(keys);
                return val;
            }

            // Materialize fields
            switch (val->type) {
                case TYPE_MAPFILTER:
                    prm = filter_collect(AS_LIST(val)[0], AS_LIST(val)[1]);
                    drop_obj(val);
                    val = prm;
                    break;
                case TYPE_MAPGROUP:
                    prm = aggr_collect(AS_LIST(val)[0], AS_LIST(val)[1]);
                    drop_obj(val);
                    val = prm;
                    break;
                default:
                    prm = ray_value(val);
                    drop_obj(val);
                    val = prm;
                    break;
            }

            if (IS_ERR(val)) {
                res->len = i;
                drop_obj(res);
                drop_obj(keys);
                return val;
            }

            AS_LIST(res)[i] = val;
        }

        ctx->query_fields = keys;
        ctx->query_values = res;

        timeit_tick("apply mappings");

        return NULL_OBJ;
    }

    drop_obj(keys);

    return NULL_OBJ;
}

obj_p select_collect_fields(query_ctx_p ctx) {
    i64_t i, l;
    obj_p prm, sym, val, keys, res;

    // Already collected by mappings
    if (!is_null(ctx->query_fields))
        return NULL_OBJ;

    // Groupings
    if (!is_null(ctx->group_fields)) {
        keys = ray_except(AS_LIST(ctx->table)[0], ctx->group_fields);
        l = keys->len;
        res = LIST(l);

        for (i = 0; i < l; i++) {
            sym = at_idx(keys, i);
            prm = ray_get(sym);
            drop_obj(sym);

            if (IS_ERR(prm)) {
                res->len = i;
                drop_obj(res);
                drop_obj(keys);
                return prm;
            }

            val = aggr_first(AS_LIST(prm)[0], AS_LIST(prm)[1]);
            drop_obj(prm);

            if (IS_ERR(val)) {
                res->len = i;
                drop_obj(res);
                drop_obj(keys);
                return val;
            }

            AS_LIST(res)[i] = val;
        }

        ctx->query_fields = keys;
        ctx->query_values = res;

        timeit_tick("collect fields");

        return NULL_OBJ;
    }

    keys = clone_obj(AS_LIST(ctx->table)[0]);
    l = keys->len;
    res = LIST(l);

    for (i = 0; i < l; i++) {
        sym = at_idx(keys, i);
        prm = ray_get(sym);
        drop_obj(sym);

        switch (prm->type) {
            case TYPE_MAPFILTER:
                val = filter_collect(AS_LIST(prm)[0], AS_LIST(prm)[1]);
                drop_obj(prm);
                break;
            default:
                val = ray_value(prm);
                drop_obj(prm);
                break;
        }

        if (IS_ERR(val)) {
            res->len = i;
            drop_obj(res);
            drop_obj(keys);
            return val;
        }

        AS_LIST(res)[i] = val;
    }

    ctx->query_fields = keys;
    ctx->query_values = res;

    timeit_tick("collect fields");

    return NULL_OBJ;
}

obj_p select_build_table(query_ctx_p ctx) {
    i64_t i, l, m;
    obj_p take, res, keys, vals;

    switch (ctx->group_fields->type) {
        case -TYPE_SYMBOL:  // Grouped by one column
            keys = ray_concat(ctx->group_fields, ctx->query_fields);
            l = ctx->query_values->len;
            vals = LIST(l + 1);
            AS_LIST(vals)[0] = clone_obj(ctx->group_values);

            for (i = 0; i < l; i++)
                AS_LIST(vals)[i + 1] = clone_obj(AS_LIST(ctx->query_values)[i]);

            break;
        case TYPE_SYMBOL:  // Grouped by multiple columns
            keys = ray_concat(ctx->group_fields, ctx->query_fields);
            l = ctx->group_values->len;
            m = ctx->query_values->len;
            vals = LIST(l + m);

            for (i = 0; i < l; i++)
                AS_LIST(vals)[i] = clone_obj(AS_LIST(ctx->group_values)[i]);

            for (i = 0; i < m; i++)
                AS_LIST(vals)[i + l] = clone_obj(AS_LIST(ctx->query_values)[i]);

            break;
        default:
            keys = clone_obj(ctx->query_fields);
            vals = clone_obj(ctx->query_values);
    }

    res = ray_table(keys, vals);
    drop_obj(keys);
    drop_obj(vals);

    if (ctx->take != NULL_OBJ) {
        take = ray_take(res, ctx->take);
        drop_obj(res);
        res = take;
    }

    timeit_tick("build table");

    return res;
}

obj_p ray_select(obj_p obj) {
    obj_p res;
    struct query_ctx_t ctx;

    query_ctx_init(&ctx);

    if (obj->type != TYPE_DICT)
        return err_type(0, 0, 0);

    if (AS_LIST(obj)[0]->type != TYPE_SYMBOL)
        return err_type(0, 0, 0);

    timeit_span_start("select");

    // Fetch table - ctx.table is set, resolve() will find columns via query_ctx
    res = select_fetch_table(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Apply filters
    res = select_apply_filters(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Apply groupping
    res = select_apply_groupings(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Apply mappings
    res = select_apply_mappings(obj, &ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Collect fields
    res = select_collect_fields(&ctx);
    if (IS_ERR(res))
        goto cleanup;

    // Build result table
    res = select_build_table(&ctx);

cleanup:
    query_ctx_destroy(&ctx);
    timeit_span_end("select");

    return res;
}
