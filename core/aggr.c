/*
 *   Copyright (c) 2024 Anton Kundenko <singaraiona@gmail.com>
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

#include "aggr.h"
#include "math.h"
#include "ops.h"
#include "error.h"
#include "hash.h"
#include "util.h"
#include "items.h"
#include "unary.h"
#include "group.h"
#include "string.h"
#include "runtime.h"
#include "index.h"
#include "cmp.h"
#include "pool.h"

#define AGGR_ITER(index, len, offset, val, res, incoerse, outcoerse, ini, aggr)      \
    ({                                                                               \
        i64_t $i, $x, $y, $n;                                                        \
        i64_t *group_ids, *source, *filter, shift;                                   \
        index_type_t index_type;                                                     \
        incoerse##_t *$in;                                                           \
        outcoerse##_t *$out;                                                         \
        index_type = index_group_type(index);                                        \
        $n = (index_type == INDEX_TYPE_PARTEDCOMMON) ? 1 : index_group_count(index); \
        group_ids = index_group_ids(index);                                          \
        $in = __AS_##incoerse(val);                                                  \
        $out = __AS_##outcoerse(res);                                                \
        for ($y = 0; $y < $n; $y++) {                                                \
            ini;                                                                     \
        }                                                                            \
        filter = index_group_filter_ids(index);                                      \
        switch (index_type) {                                                        \
            case INDEX_TYPE_SHIFT:                                                   \
                source = index_group_source(index);                                  \
                shift = index_group_shift(index);                                    \
                if (filter != NULL) {                                                \
                    for ($i = 0; $i < len; $i++) {                                   \
                        $x = filter[$i + offset];                                    \
                        $y = group_ids[source[$x] - shift];                          \
                        aggr;                                                        \
                    }                                                                \
                } else {                                                             \
                    for ($i = 0; $i < len; $i++) {                                   \
                        $x = $i + offset;                                            \
                        $y = group_ids[source[$x] - shift];                          \
                        aggr;                                                        \
                    }                                                                \
                }                                                                    \
                break;                                                               \
            case INDEX_TYPE_IDS:                                                     \
                if (filter != NULL) {                                                \
                    for ($i = 0; $i < len; $i++) {                                   \
                        $x = filter[$i + offset];                                    \
                        $y = group_ids[$i + offset];                                 \
                        aggr;                                                        \
                    }                                                                \
                } else {                                                             \
                    for ($i = 0; $i < len; $i++) {                                   \
                        $x = $i + offset;                                            \
                        $y = group_ids[$x];                                          \
                        aggr;                                                        \
                    }                                                                \
                }                                                                    \
                break;                                                               \
            case INDEX_TYPE_PARTEDCOMMON:                                            \
                for ($i = 0; $i < len; $i++) {                                       \
                    $x = $i + offset;                                                \
                    $y = 0;                                                          \
                    aggr;                                                            \
                }                                                                    \
                break;                                                               \
        }                                                                            \
    })

#define AGGR_COLLECT(parts, groups, incoerse, outcoerse, aggr) \
    ({                                                         \
        i64_t $x, $y, $i, $j, $l;                              \
        incoerse##_t *$in;                                     \
        outcoerse##_t *$out;                                   \
        obj_p $res;                                            \
        $res = clone_obj(AS_LIST(parts)[0]);                   \
        $l = parts->len;                                       \
        $out = __AS_##outcoerse($res);                         \
        for ($i = 1; $i < $l; $i++) {                          \
            $in = __AS_##incoerse(AS_LIST(parts)[$i]);         \
            for ($j = 0; $j < groups; $j++) {                  \
                $x = $j;                                       \
                $y = $j;                                       \
                aggr;                                          \
            }                                                  \
        }                                                      \
        $res;                                                  \
    })

#define PARTED_MAP(groups, val, index, preaggr, incoerse, outcoerse, postaggr)                        \
    ({                                                                                                \
        i64_t $$i, $$j, $$l;                                                                          \
        obj_p $$parts, $$res, $$filter, $$v;                                                          \
        $$l = val->len;                                                                               \
        $$filter = index_group_filter(index);                                                         \
        $$res = __v_##outcoerse(groups);                                                              \
        for ($$i = 0, $$j = 0; $$i < $$l; $$i++) {                                                    \
            if ($$filter == NULL_OBJ || AS_LIST($$filter)[$$i] != NULL_OBJ) {                         \
                $$parts = aggr_map(preaggr, AS_LIST(val)[$$i], AS_LIST(val)[$$i]->type, index);       \
                $$v = AGGR_COLLECT($$parts, 1, incoerse, outcoerse, postaggr);                        \
                drop_obj($$parts);                                                                    \
                memcpy(__AS_##outcoerse($$res) + $$j++, __AS_##incoerse($$v), __SIZE_OF_##outcoerse); \
                drop_obj($$v);                                                                        \
            }                                                                                         \
        }                                                                                             \
        $$res;                                                                                        \
    })

obj_p aggr_map(raw_p aggr, obj_p val, i8_t outype, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res;
    index_type_t index_type;
    raw_p argv[6];

    if (outype > TYPE_MAPLIST && outype < TYPE_TABLE)
        outype = AS_LIST(val)[0]->type;

    index_type = index_group_type(index);
    group_count = index_group_count(index);
    group_len = (index_type == INDEX_TYPE_PARTEDCOMMON) ? val->len : index_group_len(index);
    out_len = (index_type == INDEX_TYPE_PARTEDCOMMON) ? 1 : group_count;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = (index_type == INDEX_TYPE_PARTEDCOMMON) ? vector(outype, 1) : vector(outype, out_len);
        res = pool_call_task_fn(aggr, 5, argv);

        return vn_list(1, res);
    }

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, aggr, 5, chunk, i * chunk, val, index, vector(outype, out_len));

    pool_add_task(pool, aggr, 5, l - i * chunk, i * chunk, val, index, vector(outype, out_len));

    return pool_run(pool);
}

nil_t destroy_partial_result(obj_p res) {
    res->len = 0;
    drop_obj(res);
}

obj_p aggr_first_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = NULL_I64,
                      if ($out[$y] == NULL_I64) $out[$y] = $in[$x]);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = NULL_F64,
                      if (ISNANF64($out[$y])) $out[$y] = $in[$x]);
            return res;
        case TYPE_GUID:
            AGGR_ITER(index, len, offset, val, res, guid, guid, memset($out[$y], 0, sizeof(guid_t)),
                      if (memcmp($out[$y], NULL_GUID, sizeof(guid_t)) == 0) memcpy($out[$y], $in[$x], sizeof(guid_t)));
            return res;
        case TYPE_LIST:
            AGGR_ITER(index, len, offset, val, res, list, list, $out[$y] = NULL_OBJ,
                      if ($out[$y] == NULL_OBJ) $out[$y] = clone_obj($in[$x]));
            return res;
        default:
            destroy_partial_result(res);
            return error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_first(obj_p val, obj_p index) {
    i64_t i, j, n, l;
    i64_t *xo, *xe;
    obj_p parts, res, ek, filter, sym;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i64, i64, if ($out[$y] == NULL_I64) $out[$y] = $in[$x]);
            res->type = val->type;
            drop_obj(parts);

            if (val->type == TYPE_ENUM) {
                ek = ray_key(val);
                sym = ray_get(ek);
                drop_obj(ek);

                if (IS_ERR(sym)) {
                    drop_obj(res);
                    return sym;
                }

                if (is_null(sym) || sym->type != TYPE_SYMBOL) {
                    drop_obj(sym);
                    drop_obj(res);
                    return error(ERR_TYPE, "first: can not resolve an enum");
                }

                xe = AS_SYMBOL(sym);
                xo = AS_SYMBOL(res);
                for (i = 0; i < n; i++)
                    xo[i] = xe[xo[i]];

                drop_obj(sym);
                res->type = TYPE_SYMBOL;
            }

            return res;
        case TYPE_F64:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, f64, f64, if (ISNANF64($out[$y])) $out[$y] = $in[$x]);
            drop_obj(parts);
            return res;
        case TYPE_GUID:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, guid, guid,
                               if (memcmp($out[$y], NULL_GUID, sizeof(guid_t)) == 0)
                                   memcpy($out[$y], $in[$x], sizeof(guid_t)));
            drop_obj(parts);
            return res;
        case TYPE_LIST:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, list, list, if ($out[$y] == NULL_OBJ) $out[$y] = clone_obj($in[$x]));
            drop_obj(parts);
            return res;
        // TODO: implement anymap
        // case TYPE_MAPLIST:
        //     res = AGGR_COLLECT(parts, n, list, list, if ($out[$y] == NULL_OBJ) $out[$y] = clone_obj($in[$x]));
        //     drop_obj(parts);
        //     return res;
        case TYPE_PARTEDLIST:
            res = LIST(n);
            for (i = 0; i < n; i++)
                AS_LIST(res)[i] = at_idx(AS_LIST(val)[i], 0);
            return res;
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
            res = vector(AS_LIST(val)[0]->type, n);
            for (i = 0; i < n; i++)
                AS_B8(res)[i] = AS_B8(AS_LIST(val)[i])[0];
            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            filter = index_group_filter(index);
            res = vector(AS_LIST(val)[0]->type, n);
            if (filter == NULL_OBJ) {
                for (i = 0; i < n; i++)
                    AS_I64(res)[i] = AS_I64(AS_LIST(val)[i])[0];
            } else {
                for (i = 0, j = 0; i < n; i++) {
                    if (AS_LIST(filter)[i] != NULL_OBJ)
                        AS_I64(res)[j++] = AS_I64(AS_LIST(val)[i])[0];
                }
                resize_obj(&res, j);
            }

            return res;
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_first_partial, f64, f64,
                              if (ISNANF64($out[$y])) $out[$y] = $in[$x]);
        case TYPE_PARTEDGUID:
            return PARTED_MAP(n, val, index, (raw_p)aggr_first_partial, guid, guid,
                              if (memcmp($out[$y], NULL_GUID, sizeof(guid_t)) == 0)
                                  memcpy($out[$y], $in[$x], sizeof(guid_t)));
        case TYPE_MAPCOMMON:
            filter = index_group_filter(index);
            if (filter == NULL_OBJ)
                return clone_obj(AS_LIST(val)[0]);

            res = vector(AS_LIST(val)[0]->type, n);
            l = filter->len;

            for (i = 0, j = 0; i < l; i++) {
                if (AS_LIST(filter)[i] != NULL_OBJ)
                    AS_DATE(res)[j++] = AS_DATE(AS_LIST(val)[0])[i];
            }

            return res;
        default:
            return error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_last_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = NULL_I64,
                      if ($in[$x] != NULL_I64) $out[$y] = $in[$x]);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = NULL_F64,
                      if (!ISNANF64($in[$x])) $out[$y] = $in[$x]);
            return res;
        case TYPE_GUID:
            AGGR_ITER(index, len, offset, val, res, guid, guid, memset($out[$y], 0, sizeof(guid_t)),
                      if (memcmp($in[$x], NULL_GUID, sizeof(guid_t)) != 0) memcpy($out[$y], $in[$x], sizeof(guid_t)));
            return res;
        case TYPE_LIST:
            AGGR_ITER(
                index, len, offset, val, res, list, list, $out[$y] = NULL_OBJ, if ($in[$x] != NULL_OBJ) {
                    drop_obj($out[$y]);
                    $out[$y] = clone_obj($in[$x]);
                });
            return res;
        default:
            destroy_partial_result(res);
            return error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_last(obj_p val, obj_p index) {
    i64_t i, n;
    i64_t *xo, *xe;
    obj_p parts, res, ek, sym;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            parts = aggr_map((raw_p)aggr_last_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i64, i64, if ($out[$y] == NULL_I64) $out[$y] = $in[$x]);
            drop_obj(parts);
            if (val->type == TYPE_ENUM) {
                ek = ray_key(val);
                sym = ray_get(ek);
                drop_obj(ek);

                if (IS_ERR(sym)) {
                    drop_obj(res);
                    return sym;
                }

                if (is_null(sym) || sym->type != TYPE_SYMBOL) {
                    drop_obj(sym);
                    drop_obj(res);
                    return error(ERR_TYPE, "first: can not resolve an enum");
                }

                xe = AS_SYMBOL(sym);
                xo = AS_I64(res);
                for (i = 0; i < n; i++)
                    xo[i] = xe[xo[i]];

                drop_obj(sym);
            }

            return res;
        case TYPE_F64:
            parts = aggr_map((raw_p)aggr_last_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, f64, f64, if (ISNANF64($out[$y])) $out[$y] = $in[$x]);
            drop_obj(parts);
            return res;
        case TYPE_GUID:
            parts = aggr_map((raw_p)aggr_last_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, guid, guid,
                               if (memcmp($out[$y], NULL_GUID, sizeof(guid_t)) == 0)
                                   memcpy($out[$y], $in[$x], sizeof(guid_t)));
            drop_obj(parts);
            return res;
        case TYPE_LIST:
            parts = aggr_map((raw_p)aggr_last_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, list, list, if ($out[$y] == NULL_OBJ) $out[$y] = clone_obj($in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_PARTEDLIST:
            res = LIST(n);
            for (i = 0; i < n; i++)
                AS_LIST(res)[i] = at_idx(AS_LIST(val)[i], -1);
            return res;
        default:
            return error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_sum_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = 0, $out[$y] = ADDI64($out[$y], $in[$x]));
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = 0.0, $out[$y] = ADDF64($out[$y], $in[$x]));
            return res;
        default:
            destroy_partial_result(res);
            return error(ERR_TYPE, "sum partial: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_sum(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I64:
            parts = aggr_map((raw_p)aggr_sum_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i64, i64, $out[$y] = ADDI64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_F64:
            parts = aggr_map((raw_p)aggr_sum_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, f64, f64, $out[$y] = ADDF64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_PARTEDI64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_sum_partial, i64, i64, $out[$y] = ADDI64($out[$y], $in[$x]));
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_sum_partial, f64, f64, $out[$y] = ADDF64($out[$y], $in[$x]));
        default:
            return error(ERR_TYPE, "sum: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = 0, $out[$y] = MAXI64($out[$y], $in[$x]));
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = 0.0, $out[$y] = MAXF64($out[$y], $in[$x]));
            return res;
        default:
            destroy_partial_result(res);
            return error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I64:
            parts = aggr_map((raw_p)aggr_max_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i64, i64, $out[$y] = MAXI64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_F64:
            parts = aggr_map((raw_p)aggr_max_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, f64, f64, $out[$y] = MAXF64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_PARTEDI64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_max_partial, i64, i64, $out[$y] = MAXI64($out[$y], $in[$x]));
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_max_partial, f64, f64, $out[$y] = MAXF64($out[$y], $in[$x]));
        default:
            return error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = 0, $out[$y] = MINI64($out[$y], $in[$x]));
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = 0.0, $out[$y] = MINF64($out[$y], $in[$x]));
            return res;
        default:
            res->len = 0;
            drop_obj(res);
            return error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);
    parts = aggr_map((raw_p)aggr_max_partial, val, val->type, index);
    if (IS_ERR(parts))
        return parts;
    switch (val->type) {
        case TYPE_I64:
            res = AGGR_COLLECT(parts, n, i64, i64, $out[$y] = MINI64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_F64:
            res = AGGR_COLLECT(parts, n, f64, f64, $out[$y] = MINF64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        default:
            drop_obj(parts);
            return error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_count_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    UNUSED(val);
    switch (val->type) {
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = 0, {
                UNUSED($in);
                $out[$y]++;
            });
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, i64, $out[$y] = 0, {
                UNUSED($in);
                $out[$y]++;
            });
            return res;
        case TYPE_GUID:
            AGGR_ITER(index, len, offset, val, res, guid, i64, $out[$y] = 0, {
                UNUSED($in);
                $out[$y]++;
            });
            return res;
        case TYPE_LIST:
            AGGR_ITER(index, len, offset, val, res, list, i64, $out[$y] = 0, {
                UNUSED($in);
                $out[$y]++;
            });
            return res;
        default:
            res->len = 0;
            drop_obj(res);
            return error(ERR_TYPE, "count: unsupported type: '%s'", type_name(val->type));
    }

    return res;
}

obj_p aggr_count(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);
    parts = aggr_map((raw_p)aggr_count_partial, val, TYPE_I64, index);
    if (IS_ERR(parts))
        return parts;
    res = AGGR_COLLECT(parts, n, i64, i64, $out[$y] += $in[$x]);
    drop_obj(parts);

    return res;
}

obj_p aggr_avg(obj_p val, obj_p index) {
    i64_t i, l;
    i64_t *xi, *ci;
    f64_t *xf, *fo;
    obj_p res, sums, cnts;

    switch (val->type) {
        case TYPE_I64:
            sums = aggr_sum(val, index);
            cnts = aggr_count(val, index);

            xi = AS_I64(sums);
            ci = AS_I64(cnts);

            l = sums->len;
            res = F64(l);
            fo = AS_F64(res);

            for (i = 0; i < l; i++)
                fo[i] = FDIVI64(xi[i], ci[i]);

            drop_obj(sums);
            drop_obj(cnts);

            return res;
        case TYPE_F64:
            sums = aggr_sum(val, index);
            cnts = aggr_count(val, index);

            xf = AS_F64(sums);
            ci = AS_I64(cnts);

            l = sums->len;
            res = F64(l);
            fo = AS_F64(res);

            for (i = 0; i < l; i++)
                fo[i] = FDIVF64(xf[i], ci[i]);

            drop_obj(sums);
            drop_obj(cnts);

            return res;

        default:
            return error(ERR_TYPE, "avg: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_med(obj_p val, obj_p index) {
    obj_p res;

    // TODO: implement incremental median
    val = aggr_collect(val, index);
    res = ray_med(val);
    drop_obj(val);

    return res;
}

obj_p aggr_dev(obj_p val, obj_p index) {
    obj_p res;

    // TODO: implement incremental stddev
    val = aggr_collect(val, index);
    res = ray_dev(val);
    drop_obj(val);

    return res;
}

obj_p aggr_collect(obj_p val, obj_p index) {
    i64_t l, m, n;
    i64_t *cnts;
    obj_p cnt, k, v, res;

    cnt = aggr_count(val, index);
    if (IS_ERR(cnt))
        return cnt;

    cnts = AS_I64(cnt);
    n = cnt->len;
    l = index_group_len(index);

    switch (val->type) {
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            res = LIST(n);
            AGGR_ITER(
                index, l, 0, val, res, i64, list,
                {
                    m = cnts[$y];
                    $out[$y] = vector(val->type, m);
                    $out[$y]->len = 0;
                },
                AS_I64($out[$y])[$out[$y]->len++] = $in[$x]);
            drop_obj(cnt);

            return res;
        case TYPE_F64:
            res = LIST(n);
            AGGR_ITER(
                index, l, 0, val, res, f64, list,
                {
                    m = cnts[$y];
                    $out[$y] = vector(val->type, m);
                    $out[$y]->len = 0;
                },
                AS_F64($out[$y])[$out[$y]->len++] = $in[$x]);
            drop_obj(cnt);

            return res;

        case TYPE_ENUM:
            k = ray_key(val);
            if (IS_ERR(k))
                return k;

            v = ray_get(k);
            drop_obj(k);

            if (IS_ERR(v))
                return v;

            if (v->type != TYPE_SYMBOL) {
                drop_obj(cnt);
                drop_obj(v);
                return error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", type_name(v->type));
            }

            res = LIST(n);
            AGGR_ITER(
                index, l, 0, val, res, i64, list,
                {
                    m = cnts[$y];
                    $out[$y] = SYMBOL(m);
                    $out[$y]->len = 0;
                },
                AS_SYMBOL($out[$y])[$out[$y]->len++] = AS_SYMBOL(v)[$in[$x]]);
            drop_obj(v);
            drop_obj(cnt);

            return res;
        case TYPE_LIST:
            res = LIST(n);
            AGGR_ITER(
                index, l, 0, val, res, list, list,
                {
                    m = cnts[$y];
                    $out[$y] = LIST(m);
                    $out[$y]->len = 0;
                },
                AS_LIST($out[$y])[$out[$y]->len++] = clone_obj($in[$x]));
            drop_obj(cnt);

            return res;
        default:
            drop_obj(cnt);
            THROW(ERR_TYPE, "collect: unsupported type: '%s", type_name(val->type));
    }
}

obj_p aggr_row_index(obj_p val, obj_p index) {
    i64_t l, m, n;
    i64_t *cnts;
    obj_p cnt, res;

    cnt = aggr_count(val, index);
    if (IS_ERR(cnt))
        return cnt;

    cnts = AS_I64(cnt);
    n = cnt->len;
    l = index_group_len(index);

    res = LIST(n);
    AGGR_ITER(
        index, l, 0, val, res, i64, list,
        {
            UNUSED($in);
            m = cnts[$y];
            $out[$y] = I64(m);
            $out[$y]->len = 0;
        },
        AS_I64($out[$y])[$out[$y]->len++] = $x);
    drop_obj(cnt);

    return res;
}
