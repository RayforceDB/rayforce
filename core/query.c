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

obj_p remap_group(query_ctx_p ctx) { return group_map(ctx->table, NULL_OBJ); }

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
                return err_type(0, 0, 0, 0);

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
    ctx->table = NULL_OBJ;
    ctx->take = NULL_OBJ;
    ctx->filter = NULL_OBJ;
    ctx->groupby = NULL_OBJ;
    ctx->parent = vm->query_ctx;
    vm->query_ctx = ctx;
}

nil_t query_ctx_destroy(query_ctx_p ctx) {
    VM->query_ctx = ctx->parent;

    drop_obj(ctx->table);
    drop_obj(ctx->take);
    drop_obj(ctx->filter);
    drop_obj(ctx->groupby);
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
        return err_type(TYPE_TABLE, actual_type, 0, symbols_intern("from", 4));
    }

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

        // Store key columns for fused hash-aggregate
        // groupby is either a single column or a list of columns
        if (groupby->type == TYPE_LIST) {
            ctx->groupby = clone_obj(groupby);
        } else {
            ctx->groupby = LIST(1);
            AS_LIST(ctx->groupby)[0] = clone_obj(groupby);
        }

        prm = remap_group(ctx);

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

        timeit_tick("apply mappings");

        return table(keys, res);
    }

    drop_obj(keys);

    return NULL_OBJ;
}

obj_p ray_select(obj_p obj) {
    obj_p res;
    struct query_ctx_t ctx;

    query_ctx_init(&ctx);

    if (obj->type != TYPE_DICT)
        return err_type(0, 0, 0, 0);

    if (AS_LIST(obj)[0]->type != TYPE_SYMBOL)
        return err_type(0, 0, 0, 0);

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

cleanup:
    query_ctx_destroy(&ctx);
    timeit_span_end("select");

    return res;
}
