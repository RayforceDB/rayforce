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

#include <math.h>
#include "aggr.h"
#include "math.h"
#include "ops.h"
#include "error.h"
#include "rayforce.h"
#include "util.h"
#include "items.h"
#include "unary.h"
#include "string.h"
#include "runtime.h"
#include "index.h"
#include "pool.h"
#include "order.h"

i64_t indexr_bin_i32_(i32_t val, i32_t vals[], i64_t offset, i64_t len) {
    i64_t left, right, mid, idx;
    left = 0, right = len - 1, idx = 0;
    vals += offset;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[mid] <= val) {
            idx = mid;
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    return idx + offset;
}

i64_t indexl_bin_i32_(i32_t val, i32_t vals[], i64_t offset, i64_t len) {
    i64_t left, right, mid, idx;
    left = 0, right = len - 1, idx = 0;
    vals += offset;
    while (left <= right) {
        mid = left + (right - left) / 2;
        if (vals[mid] < val) {
            left = mid + 1;
        } else {
            idx = mid;
            right = mid - 1;
        }
    }

    return idx + offset;
}

#define AGGR_ITER(Index, Len, Offset, Val, Res, Incoerce, Outcoerse, Ini, Aggr, Null)                  \
    ({                                                                                                 \
        i64_t $i, $x, $y, $n, $o, $li, $ri, $fi, $ti, $kl, $kr, $it;                                   \
        i64_t *group_ids, *source, *filter, shift;                                                     \
        obj_p $rn;                                                                                     \
        index_type_t index_type;                                                                       \
        Incoerce##_t *$in;                                                                             \
        Outcoerse##_t *$out;                                                                           \
        index_type = index_group_type(Index);                                                          \
        $n = (index_type == INDEX_TYPE_PARTEDCOMMON) ? 1 : index_group_count(Index);                   \
        if (index_type == INDEX_TYPE_WINDOW)                                                           \
            $n = Len;                                                                                  \
        $o = (index_type == INDEX_TYPE_WINDOW) ? Offset : 0;                                           \
        group_ids = index_group_ids(Index);                                                            \
        $in = __AS_##Incoerce(Val);                                                                    \
        $out = __AS_##Outcoerse(Res);                                                                  \
        for ($y = $o; $y < $n + $o; ++$y) {                                                            \
            Ini;                                                                                       \
        }                                                                                              \
        filter = index_group_filter_ids(Index);                                                        \
        switch (index_type) {                                                                          \
            case INDEX_TYPE_SHIFT:                                                                     \
                source = index_group_source(Index);                                                    \
                shift = index_group_shift(Index);                                                      \
                if (filter != NULL) {                                                                  \
                    for ($i = 0; $i < Len; ++$i) {                                                     \
                        $x = filter[$i + Offset];                                                      \
                        $y = group_ids[source[$x] - shift];                                            \
                        Aggr;                                                                          \
                    }                                                                                  \
                } else {                                                                               \
                    for ($i = 0; $i < Len; ++$i) {                                                     \
                        $x = $i + Offset;                                                              \
                        $y = group_ids[source[$x] - shift];                                            \
                        Aggr;                                                                          \
                    }                                                                                  \
                }                                                                                      \
                break;                                                                                 \
            case INDEX_TYPE_IDS:                                                                       \
                if (filter != NULL) {                                                                  \
                    for ($i = 0; $i < Len; ++$i) {                                                     \
                        $x = filter[$i + Offset];                                                      \
                        $y = group_ids[$i + Offset];                                                   \
                        Aggr;                                                                          \
                    }                                                                                  \
                } else {                                                                               \
                    for ($i = 0; $i < Len; ++$i) {                                                     \
                        $x = $i + Offset;                                                              \
                        $y = group_ids[$x];                                                            \
                        Aggr;                                                                          \
                    }                                                                                  \
                }                                                                                      \
                break;                                                                                 \
            case INDEX_TYPE_PARTEDCOMMON:                                                              \
                for ($i = 0; $i < Len; ++$i) {                                                         \
                    $x = $i + Offset;                                                                  \
                    $y = 0;                                                                            \
                    Aggr;                                                                              \
                }                                                                                      \
                break;                                                                                 \
            case INDEX_TYPE_WINDOW:                                                                    \
                $it = index_group_meta(Index)->i64;                                                    \
                for ($i = Offset; $i < Offset + Len; ++$i) {                                           \
                    $y = $i;                                                                           \
                    $rn = AS_LIST(AS_LIST(Index)[5])[$i];                                              \
                    if ($rn != NULL_OBJ) {                                                             \
                        $fi = AS_I64($rn)[0];                                                          \
                        $ti = AS_I64($rn)[1];                                                          \
                        $kl = AS_I32(AS_LIST(AS_LIST(Index)[4])[0])[$i];                               \
                        $kr = AS_I32(AS_LIST(AS_LIST(Index)[4])[1])[$i];                               \
                        if ($it == 0) {                                                                \
                            $li = indexr_bin_i32_($kl, AS_I32(AS_LIST(Index)[3]), $fi, $ti - $fi + 1); \
                        } else {                                                                       \
                            $li = indexl_bin_i32_($kl, AS_I32(AS_LIST(Index)[3]), $fi, $ti - $fi + 1); \
                        }                                                                              \
                        $ri = indexr_bin_i32_($kr, AS_I32(AS_LIST(Index)[3]), $fi, $ti - $fi + 1);     \
                    }                                                                                  \
                    if ($rn == NULL_OBJ || AS_I32(AS_LIST(Index)[3])[$li] > $kr ||                     \
                        ($it == 1 && AS_I32(AS_LIST(Index)[3])[$ri] < $kl)) {                          \
                        Null;                                                                          \
                    } else {                                                                           \
                        for ($x = $li; $x <= $ri; ++$x) {                                              \
                            Aggr;                                                                      \
                        }                                                                              \
                    }                                                                                  \
                }                                                                                      \
                break;                                                                                 \
        }                                                                                              \
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

#define PARTED_MAP(groups, val, index, preaggr, incoerse, outcoerse, postaggr)                                       \
    ({                                                                                                               \
        i64_t $$i, $$j, $$l;                                                                                         \
        b8_t $$first;                                                                                                \
        obj_p $$parts, $$res, $$filter, $$v, $$pfilter, $$pdata, $$pindex;                                           \
        $$l = val->len;                                                                                              \
        $$filter = index_group_filter(index);                                                                        \
        $$res = __v_##outcoerse(groups);                                                                             \
        $$first = B8_TRUE;                                                                                           \
        if (groups == 1 && $$filter == NULL_OBJ) {                                                                   \
            /* Aggregate all partitions into single result, no filter */                                             \
            for ($$i = 0; $$i < $$l; $$i++) {                                                                        \
                $$parts = aggr_map(preaggr, AS_LIST(val)[$$i], AS_LIST(val)[$$i]->type, index);                      \
                $$v = AGGR_COLLECT($$parts, 1, incoerse, outcoerse, postaggr);                                       \
                drop_obj($$parts);                                                                                   \
                if ($$first) {                                                                                       \
                    memcpy(__AS_##outcoerse($$res), __AS_##incoerse($$v), __SIZE_OF_##outcoerse);                    \
                    $$first = B8_FALSE;                                                                              \
                } else {                                                                                             \
                    outcoerse##_t *$out = __AS_##outcoerse($$res);                                                   \
                    incoerse##_t *$in = __AS_##incoerse($$v);                                                        \
                    i64_t $x = 0, $y = 0;                                                                            \
                    postaggr;                                                                                        \
                }                                                                                                    \
                drop_obj($$v);                                                                                       \
            }                                                                                                        \
        } else if ($$filter != NULL_OBJ && $$filter->type == TYPE_PARTEDI64) {                                       \
            /* Parted filter - handle per-partition indices */                                                       \
            $$pindex = vn_list(7, i64(INDEX_TYPE_PARTEDCOMMON), i64(1), NULL_OBJ, i64(NULL_I64), NULL_OBJ, NULL_OBJ, \
                               NULL_OBJ);                                                                            \
            for ($$i = 0, $$j = 0; $$i < $$l; $$i++) {                                                               \
                $$pfilter = AS_LIST($$filter)[$$i];                                                                  \
                if ($$pfilter == NULL_OBJ)                                                                           \
                    continue;                                                                                        \
                if ($$pfilter->type > 0 && $$pfilter->len == 0)                                                      \
                    continue;                                                                                        \
                $$pdata = AS_LIST(val)[$$i];                                                                         \
                if ($$pfilter->type == -TYPE_I64 && $$pfilter->i64 == -1) {                                          \
                    /* All rows match - use partition data as-is */                                                  \
                    $$parts = aggr_map(preaggr, $$pdata, $$pdata->type, $$pindex);                                   \
                } else {                                                                                             \
                    /* Specific indices - filter data first */                                                       \
                    $$pdata = at_ids($$pdata, AS_I64($$pfilter), $$pfilter->len);                                    \
                    $$parts = aggr_map(preaggr, $$pdata, $$pdata->type, $$pindex);                                   \
                    drop_obj($$pdata);                                                                               \
                }                                                                                                    \
                $$v = AGGR_COLLECT($$parts, 1, incoerse, outcoerse, postaggr);                                       \
                drop_obj($$parts);                                                                                   \
                if (groups == 1) {                                                                                   \
                    if ($$first) {                                                                                   \
                        memcpy(__AS_##outcoerse($$res), __AS_##incoerse($$v), __SIZE_OF_##outcoerse);                \
                        $$first = B8_FALSE;                                                                          \
                    } else {                                                                                         \
                        outcoerse##_t *$out = __AS_##outcoerse($$res);                                               \
                        incoerse##_t *$in = __AS_##incoerse($$v);                                                    \
                        i64_t $x = 0, $y = 0;                                                                        \
                        postaggr;                                                                                    \
                    }                                                                                                \
                } else {                                                                                             \
                    memcpy(__AS_##outcoerse($$res) + $$j++, __AS_##incoerse($$v), __SIZE_OF_##outcoerse);            \
                }                                                                                                    \
                drop_obj($$v);                                                                                       \
            }                                                                                                        \
            drop_obj($$pindex);                                                                                      \
        } else {                                                                                                     \
            /* No filter or simple filter - one result per partition */                                              \
            for ($$i = 0, $$j = 0; $$i < $$l; $$i++) {                                                               \
                if ($$filter == NULL_OBJ || AS_LIST($$filter)[$$i] != NULL_OBJ) {                                    \
                    $$parts = aggr_map(preaggr, AS_LIST(val)[$$i], AS_LIST(val)[$$i]->type, index);                  \
                    $$v = AGGR_COLLECT($$parts, 1, incoerse, outcoerse, postaggr);                                   \
                    drop_obj($$parts);                                                                               \
                    memcpy(__AS_##outcoerse($$res) + $$j++, __AS_##incoerse($$v), __SIZE_OF_##outcoerse);            \
                    drop_obj($$v);                                                                                   \
                }                                                                                                    \
            }                                                                                                        \
        }                                                                                                            \
        $$res;                                                                                                       \
    })

static obj_p aggr_map_other(raw_p aggr, obj_p val, i8_t outype, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    out_len = group_count;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = (raw_p)vector(outype, out_len);
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

static obj_p aggr_map_parted(raw_p aggr, obj_p val, i8_t outype, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = val->len;
    out_len = 1;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = vector(outype, out_len);
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

static obj_p aggr_map_window(raw_p aggr, obj_p val, i8_t outype, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p v, res;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    out_len = group_count;

    n = pool_get_executors_count(pool);
    res = vector(outype, out_len);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = (raw_p)res;
        res = pool_call_task_fn(aggr, 5, argv);
        return vn_list(1, res);
    }

    if (n > group_count)
        n = group_count;

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++)
        pool_add_task(pool, aggr, 5, chunk, i * chunk, val, index, clone_obj(res));

    pool_add_task(pool, aggr, 5, l - i * chunk, i * chunk, val, index, clone_obj(res));

    v = pool_run(pool);
    if (IS_ERR(v)) {
        drop_obj(res);
        return v;
    }
    drop_obj(v);
    return vn_list(1, res);
}

static obj_p aggr_map(raw_p aggr, obj_p val, i8_t outype, obj_p index) {
    if (outype > TYPE_MAPLIST && outype < TYPE_TABLE)
        outype = AS_LIST(val)[0]->type;

    switch (index_group_type(index)) {
        case INDEX_TYPE_PARTEDCOMMON:
            return aggr_map_parted(aggr, val, outype, index);
        case INDEX_TYPE_WINDOW:
            return aggr_map_window(aggr, val, outype, index);
        default:
            return aggr_map_other(aggr, val, outype, index);
    }
}

nil_t destroy_partial_result(obj_p res) {
    res->len = 0;
    drop_obj(res);
}

obj_p aggr_first_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_C8:
            AGGR_ITER(index, len, offset, val, res, u8, u8, $out[$y] = 0, if ($out[$y] == 0) $out[$y] = $in[$x],
                      $out[$y] = 0);
            return res;
        case TYPE_I16:
            AGGR_ITER(index, len, offset, val, res, i16, i16, $out[$y] = NULL_I16,
                      if ($out[$y] == NULL_I16) $out[$y] = $in[$x], $out[$y] = NULL_I16);
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            AGGR_ITER(index, len, offset, val, res, i32, i32, $out[$y] = NULL_I32,
                      if ($out[$y] == NULL_I32) $out[$y] = $in[$x], $out[$y] = NULL_I32);
            return res;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = NULL_I64,
                      if ($out[$y] == NULL_I64) $out[$y] = $in[$x], $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = NULL_F64,
                      if (ISNANF64($out[$y])) $out[$y] = $in[$x], $out[$y] = NULL_F64);
            return res;
        case TYPE_GUID:
            AGGR_ITER(index, len, offset, val, res, guid, guid, memset($out[$y], 0, sizeof(guid_t)),
                      if (memcmp($out[$y], NULL_GUID, sizeof(guid_t)) == 0) memcpy($out[$y], $in[$x], sizeof(guid_t)),
                      memset($out[$y], 0, sizeof(guid_t)));
            return res;
        case TYPE_LIST:
            AGGR_ITER(index, len, offset, val, res, list, list, $out[$y] = NULL_OBJ,
                      if ($out[$y] == NULL_OBJ) $out[$y] = clone_obj($in[$x]), $out[$y] = NULL_OBJ);
            return res;
        default:
            destroy_partial_result(res);
            THROW_TYPE1("first", val->type);
    }
}

obj_p aggr_first(obj_p val, obj_p index) {
    i64_t i, j, n, l;
    i64_t *xo, *xe;
    obj_p parts, res, ek, filter, sym;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_U8:
        case TYPE_B8:
        case TYPE_C8:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, u8, u8, if ($out[$y] == 0) $out[$y] = $in[$x]);
            res->type = val->type;
            drop_obj(parts);
            return res;
        case TYPE_I16:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i16, i16, if ($out[$y] == NULL_I16) $out[$y] = $in[$x]);
            res->type = val->type;
            drop_obj(parts);
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            parts = aggr_map((raw_p)aggr_first_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i32, i32, if ($out[$y] == NULL_I32) $out[$y] = $in[$x]);
            res->type = val->type;
            drop_obj(parts);
            return res;
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
                    return ray_error(ERR_TYPE, ERR_MSG_FIRST_ENUM);
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
            filter = index_group_filter(index);
            res = LIST(n);
            if (filter == NULL_OBJ) {
                // No filter - iterate over all partitions
                for (i = 0; i < n; i++)
                    AS_LIST(res)[i] = at_idx(AS_LIST(val)[i], 0);
            } else {
                // With filter - iterate all partitions, only take matching ones
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
                    obj_p fentry = AS_LIST(filter)[i];
                    // Check for non-null AND non-empty filter entry
                    if (fentry != NULL_OBJ) {
                        if ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0) {
                            AS_LIST(res)[j++] = at_idx(AS_LIST(val)[i], 0);
                        }
                    }
                }
                res->len = j;
            }
            return res;
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
            filter = index_group_filter(index);
            if (n == 1) {
                // Global first - return first element of first matching partition
                res = vector(AS_LIST(val)[0]->type, 1);
                l = val->len;
                for (i = 0; i < l; i++) {
                    obj_p fentry = (filter == NULL_OBJ) ? NULL_OBJ : AS_LIST(filter)[i];
                    if (filter == NULL_OBJ ||
                        (fentry != NULL_OBJ && ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0))) {
                        AS_B8(res)[0] = AS_B8(AS_LIST(val)[i])[0];
                        break;
                    }
                }
                return res;
            }
            res = vector(AS_LIST(val)[0]->type, n);
            if (filter == NULL_OBJ) {
                // No filter - iterate over all partitions
                for (i = 0; i < n; i++)
                    AS_B8(res)[i] = AS_B8(AS_LIST(val)[i])[0];
            } else {
                // With filter - iterate all partitions, only take matching ones
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
                    obj_p fentry = AS_LIST(filter)[i];
                    if (fentry != NULL_OBJ) {
                        if ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0) {
                            AS_B8(res)[j++] = AS_B8(AS_LIST(val)[i])[0];
                        }
                    }
                }
            }
            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            filter = index_group_filter(index);
            if (n == 1) {
                // Global first - return first element of first matching partition
                res = vector(AS_LIST(val)[0]->type, 1);
                l = val->len;
                for (i = 0; i < l; i++) {
                    if (filter == NULL_OBJ || AS_LIST(filter)[i] != NULL_OBJ) {
                        AS_I64(res)[0] = AS_I64(AS_LIST(val)[i])[0];
                        break;
                    }
                }
                return res;
            }
            res = vector(AS_LIST(val)[0]->type, n);
            if (filter == NULL_OBJ) {
                // No filter - one result per partition
                l = val->len;
                for (i = 0; i < l; i++)
                    AS_I64(res)[i] = AS_I64(AS_LIST(val)[i])[0];
            } else {
                // With filter - iterate all partitions, only take matching ones
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
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
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDI32:
            return PARTED_MAP(n, val, index, (raw_p)aggr_first_partial, i32, i32,
                              if ($out[$y] == NULL_I32) $out[$y] = $in[$x]);
        case TYPE_PARTEDI16:
            return PARTED_MAP(n, val, index, (raw_p)aggr_first_partial, i16, i16,
                              if ($out[$y] == NULL_I16) $out[$y] = $in[$x]);
        case TYPE_PARTEDENUM:
            filter = index_group_filter(index);
            // Get the enum key from the first partition
            ek = ray_key(AS_LIST(val)[0]);
            if (IS_ERR(ek))
                return ek;

            if (n == 1) {
                // Global first - return first element of first matching partition
                parts = I64(1);
                l = val->len;
                for (i = 0; i < l; i++) {
                    if (filter == NULL_OBJ || AS_LIST(filter)[i] != NULL_OBJ) {
                        AS_I64(parts)[0] = AS_I64(ENUM_VAL(AS_LIST(val)[i]))[0];
                        break;
                    }
                }
                res = enumerate(ek, parts);
                return res;
            }

            // Create the values vector with size n (number of groups/matching partitions)
            parts = I64(n);

            if (filter == NULL_OBJ) {
                // No filter - iterate over all partitions
                l = val->len;
                for (i = 0; i < l; i++)
                    AS_I64(parts)[i] = AS_I64(ENUM_VAL(AS_LIST(val)[i]))[0];
            } else {
                // With filter - iterate all partitions, only take matching ones
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
                    if (AS_LIST(filter)[i] != NULL_OBJ)
                        AS_I64(parts)[j++] = AS_I64(ENUM_VAL(AS_LIST(val)[i]))[0];
                }
                resize_obj(&parts, j);
            }

            res = enumerate(ek, parts);
            return res;
        case TYPE_MAPCOMMON:
            filter = index_group_filter(index);
            if (filter == NULL_OBJ)
                return clone_obj(AS_LIST(val)[0]);

            if (n == 1) {
                // Global first - return first matching value
                res = vector(AS_LIST(val)[0]->type, 1);
                l = filter->len;
                for (i = 0; i < l; i++) {
                    if (AS_LIST(filter)[i] != NULL_OBJ) {
                        AS_DATE(res)[0] = AS_DATE(AS_LIST(val)[0])[i];
                        break;
                    }
                }
                return res;
            }

            res = vector(AS_LIST(val)[0]->type, n);
            l = filter->len;

            for (i = 0, j = 0; i < l; i++) {
                if (AS_LIST(filter)[i] != NULL_OBJ)
                    AS_DATE(res)[j++] = AS_DATE(AS_LIST(val)[0])[i];
            }

            return res;
        default:
            THROW_TYPE1("first", val->type);
    }
}

obj_p aggr_last_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I16:
            AGGR_ITER(index, len, offset, val, res, i16, i16, $out[$y] = NULL_I16,
                      if ($in[$x] != NULL_I16) $out[$y] = $in[$x], $out[$y] = NULL_I16);
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            AGGR_ITER(index, len, offset, val, res, i32, i32, $out[$y] = NULL_I32,
                      if ($in[$x] != NULL_I32) $out[$y] = $in[$x], $out[$y] = NULL_I32);
            return res;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
        case TYPE_ENUM:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = NULL_I64,
                      if ($in[$x] != NULL_I64) $out[$y] = $in[$x], $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = NULL_F64,
                      if (!ISNANF64($in[$x])) $out[$y] = $in[$x], $out[$y] = NULL_F64);
            return res;
        case TYPE_GUID:
            AGGR_ITER(index, len, offset, val, res, guid, guid, memset($out[$y], 0, sizeof(guid_t)),
                      if (memcmp($in[$x], NULL_GUID, sizeof(guid_t)) != 0) memcpy($out[$y], $in[$x], sizeof(guid_t)),
                      memset($out[$y], 0, sizeof(guid_t)));
            return res;
        case TYPE_LIST:
            AGGR_ITER(
                index, len, offset, val, res, list, list, $out[$y] = NULL_OBJ,
                if ($in[$x] != NULL_OBJ) {
                    drop_obj($out[$y]);
                    $out[$y] = clone_obj($in[$x]);
                },
                $out[$y] = NULL_OBJ);
            return res;
        default:
            destroy_partial_result(res);
            THROW_TYPE1("last", val->type);
    }
}

obj_p aggr_last(obj_p val, obj_p index) {
    i64_t i, j, n, l;
    i64_t *xo, *xe;
    obj_p parts, res, ek, sym, filter;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I16:
            parts = aggr_map((raw_p)aggr_last_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i16, i16, if ($out[$y] == NULL_I16) $out[$y] = $in[$x]);
            res->type = val->type;
            drop_obj(parts);
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            parts = aggr_map((raw_p)aggr_last_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i32, i32, if ($out[$y] == NULL_I32) $out[$y] = $in[$x]);
            res->type = val->type;
            drop_obj(parts);
            return res;
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
                    return ray_error(ERR_TYPE, ERR_MSG_FIRST_ENUM);
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
            filter = index_group_filter(index);
            if (n == 1) {
                // Global last - return last element of last matching partition
                res = LIST(1);
                l = val->len;
                for (i = l - 1; i >= 0; i--) {
                    obj_p fentry = (filter == NULL_OBJ) ? NULL_OBJ : AS_LIST(filter)[i];
                    if (filter == NULL_OBJ ||
                        (fentry != NULL_OBJ && ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0))) {
                        AS_LIST(res)[0] = at_idx(AS_LIST(val)[i], -1);
                        break;
                    }
                }
                return res;
            }
            res = LIST(n);
            if (filter == NULL_OBJ) {
                // No filter - iterate over all partitions
                for (i = 0; i < n; i++)
                    AS_LIST(res)[i] = at_idx(AS_LIST(val)[i], -1);
            } else {
                // With filter - iterate all partitions, only take matching ones
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
                    obj_p fentry = AS_LIST(filter)[i];
                    if (fentry != NULL_OBJ) {
                        if ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0) {
                            AS_LIST(res)[j++] = at_idx(AS_LIST(val)[i], -1);
                        }
                    }
                }
                res->len = j;
            }
            return res;
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
            filter = index_group_filter(index);
            if (n == 1) {
                // Global last - return last element of last matching partition
                res = vector(AS_LIST(val)[0]->type, 1);
                l = val->len;
                for (i = l - 1; i >= 0; i--) {
                    obj_p fentry = (filter == NULL_OBJ) ? NULL_OBJ : AS_LIST(filter)[i];
                    if (filter == NULL_OBJ ||
                        (fentry != NULL_OBJ && ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0))) {
                        i64_t pl = AS_LIST(val)[i]->len;
                        AS_B8(res)[0] = AS_B8(AS_LIST(val)[i])[pl - 1];
                        break;
                    }
                }
                return res;
            }
            res = vector(AS_LIST(val)[0]->type, n);
            if (filter == NULL_OBJ) {
                // No filter - iterate over all partitions
                for (i = 0; i < n; i++) {
                    l = AS_LIST(val)[i]->len;
                    AS_B8(res)[i] = AS_B8(AS_LIST(val)[i])[l - 1];
                }
            } else {
                // With filter - iterate all partitions, only take matching ones
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
                    obj_p fentry = AS_LIST(filter)[i];
                    if (fentry != NULL_OBJ) {
                        if ((fentry->type == -TYPE_I64 && fentry->i64 == -1) || fentry->len > 0) {
                            i64_t pl = AS_LIST(val)[i]->len;
                            AS_B8(res)[j++] = AS_B8(AS_LIST(val)[i])[pl - 1];
                        }
                    }
                }
            }
            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            return PARTED_MAP(n, val, index, (raw_p)aggr_last_partial, i64, i64,
                              if ($out[$y] == NULL_I64) $out[$y] = $in[$x]);
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_last_partial, f64, f64,
                              if (ISNANF64($out[$y])) $out[$y] = $in[$x]);
        case TYPE_PARTEDGUID:
            return PARTED_MAP(n, val, index, (raw_p)aggr_last_partial, guid, guid,
                              if (memcmp($out[$y], NULL_GUID, sizeof(guid_t)) == 0)
                                  memcpy($out[$y], $in[$x], sizeof(guid_t)));
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDI32:
            return PARTED_MAP(n, val, index, (raw_p)aggr_last_partial, i32, i32,
                              if ($out[$y] == NULL_I32) $out[$y] = $in[$x]);
        case TYPE_PARTEDI16:
            return PARTED_MAP(n, val, index, (raw_p)aggr_last_partial, i16, i16,
                              if ($out[$y] == NULL_I16) $out[$y] = $in[$x]);
        default:
            THROW_TYPE1("last", val->type);
    }
}

obj_p aggr_sum_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I16:
            AGGR_ITER(index, len, offset, val, res, i16, i16, $out[$y] = 0, $out[$y] = ADDI16($out[$y], $in[$x]),
                      $out[$y] = NULL_I16);
            return res;
        case TYPE_I64:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = 0, $out[$y] = ADDI64($out[$y], $in[$x]),
                      $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = 0.0, $out[$y] = ADDF64($out[$y], $in[$x]),
                      $out[$y] = NULL_F64);
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            AGGR_ITER(index, len, offset, val, res, i32, i32, $out[$y] = 0, $out[$y] = ADDI32($out[$y], $in[$x]),
                      $out[$y] = NULL_I32);
            return res;
        default:
            destroy_partial_result(res);
            THROW_TYPE1("sum partial", val->type);
    }
}

obj_p aggr_sum(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I16:
            parts = aggr_map((raw_p)aggr_sum_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i16, i16, $out[$y] = ADDI16($out[$y], $in[$x]));
            res->type = val->type;
            drop_obj(parts);
            return res;
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
        case TYPE_PARTEDTIMESTAMP:
            return PARTED_MAP(n, val, index, (raw_p)aggr_sum_partial, i64, i64, $out[$y] = ADDI64($out[$y], $in[$x]));
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_sum_partial, f64, f64, $out[$y] = ADDF64($out[$y], $in[$x]));
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDI32:
            return PARTED_MAP(n, val, index, (raw_p)aggr_sum_partial, i32, i32, $out[$y] = ADDI32($out[$y], $in[$x]));
        case TYPE_PARTEDI16:
            return PARTED_MAP(n, val, index, (raw_p)aggr_sum_partial, i16, i16, $out[$y] = ADDI16($out[$y], $in[$x]));
        default:
            THROW_TYPE1("sum", val->type);
    }
}

obj_p aggr_max_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I16:
            AGGR_ITER(index, len, offset, val, res, i16, i16, $out[$y] = NULL_I16, $out[$y] = MAXI16($out[$y], $in[$x]),
                      $out[$y] = NULL_I16);
            return res;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = NULL_I64, $out[$y] = MAXI64($out[$y], $in[$x]),
                      $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = NULL_F64, $out[$y] = MAXF64($out[$y], $in[$x]),
                      $out[$y] = NULL_F64);
            return res;
        case TYPE_TIME:
        case TYPE_DATE:
            AGGR_ITER(index, len, offset, val, res, i32, i32, $out[$y] = NULL_I32, $out[$y] = MAXI32($out[$y], $in[$x]),
                      $out[$y] = NULL_I32);
            return res;
        default:
            destroy_partial_result(res);
            THROW_TYPE1("max", val->type);
    }
}

obj_p aggr_max(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I16:
            parts = aggr_map((raw_p)aggr_max_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i16, i16, $out[$y] = MAXI16($out[$y], $in[$x]));
            res->type = val->type;
            drop_obj(parts);
            return res;
        case TYPE_TIMESTAMP:
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
        case TYPE_TIME:
        case TYPE_DATE:
            parts = aggr_map((raw_p)aggr_max_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i32, i32, $out[$y] = MAXI32($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            return PARTED_MAP(n, val, index, (raw_p)aggr_max_partial, i64, i64, $out[$y] = MAXI64($out[$y], $in[$x]));
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_max_partial, f64, f64, $out[$y] = MAXF64($out[$y], $in[$x]));
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDI32:
            return PARTED_MAP(n, val, index, (raw_p)aggr_max_partial, i32, i32, $out[$y] = MAXI32($out[$y], $in[$x]));
        case TYPE_PARTEDI16:
            return PARTED_MAP(n, val, index, (raw_p)aggr_max_partial, i16, i16, $out[$y] = MAXI16($out[$y], $in[$x]));
        default:
            THROW_TYPE1("max", val->type);
    }
}

obj_p aggr_min_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I16:
            AGGR_ITER(index, len, offset, val, res, i16, i16, $out[$y] = INF_I16, $out[$y] = MINI16($out[$y], $in[$x]),
                      $out[$y] = NULL_I16);
            return res;
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = INF_I64, $out[$y] = MINI64($out[$y], $in[$x]),
                      $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = INF_F64, $out[$y] = MINF64($out[$y], $in[$x]),
                      $out[$y] = NULL_F64);
            return res;
        case TYPE_TIME:
        case TYPE_DATE:
            AGGR_ITER(index, len, offset, val, res, i32, i32, $out[$y] = INF_I32, $out[$y] = MINI32($out[$y], $in[$x]),
                      $out[$y] = NULL_I32);
            return res;
        default:
            THROW_TYPE1("min", val->type);
    }
}

obj_p aggr_min(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I16:
            parts = aggr_map((raw_p)aggr_min_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i16, i16, $out[$y] = MINI16($out[$y], $in[$x]));
            res->type = val->type;
            drop_obj(parts);
            return res;
        case TYPE_TIMESTAMP:
        case TYPE_I64:
            parts = aggr_map((raw_p)aggr_min_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i64, i64, $out[$y] = MINI64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_F64:
            parts = aggr_map((raw_p)aggr_min_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, f64, f64, $out[$y] = MINF64($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_TIME:
        case TYPE_DATE:
            parts = aggr_map((raw_p)aggr_min_partial, val, val->type, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i32, i32, $out[$y] = MINI32($out[$y], $in[$x]));
            drop_obj(parts);
            return res;
        case TYPE_PARTEDI64:
        case TYPE_PARTEDTIMESTAMP:
            return PARTED_MAP(n, val, index, (raw_p)aggr_min_partial, i64, i64, $out[$y] = MINI64($out[$y], $in[$x]));
        case TYPE_PARTEDF64:
            return PARTED_MAP(n, val, index, (raw_p)aggr_min_partial, f64, f64, $out[$y] = MINF64($out[$y], $in[$x]));
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDI32:
            return PARTED_MAP(n, val, index, (raw_p)aggr_min_partial, i32, i32, $out[$y] = MINI32($out[$y], $in[$x]));
        case TYPE_PARTEDI16:
            return PARTED_MAP(n, val, index, (raw_p)aggr_min_partial, i16, i16, $out[$y] = MINI16($out[$y], $in[$x]));
        default:
            THROW_TYPE1("min", val->type);
    }
}

obj_p aggr_count_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;
    UNUSED(val);
    switch (val->type) {
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            AGGR_ITER(
                index, len, offset, val, res, i32, i64, $out[$y] = 0,
                {
                    UNUSED($in);
                    $out[$y]++;
                },
                $out[$y] = 0);
            return res;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            AGGR_ITER(
                index, len, offset, val, res, i64, i64, $out[$y] = 0,
                {
                    UNUSED($in);
                    $out[$y]++;
                },
                $out[$y] = 0);
            return res;
        case TYPE_F64:
            AGGR_ITER(
                index, len, offset, val, res, f64, i64, $out[$y] = 0,
                {
                    UNUSED($in);
                    $out[$y]++;
                },
                $out[$y] = 0);
            return res;
        case TYPE_GUID:
            AGGR_ITER(
                index, len, offset, val, res, guid, i64, $out[$y] = 0,
                {
                    UNUSED($in);
                    $out[$y]++;
                },
                $out[$y] = 0);
            return res;
        case TYPE_LIST:
            AGGR_ITER(
                index, len, offset, val, res, list, i64, $out[$y] = 0,
                {
                    UNUSED($in);
                    $out[$y]++;
                },
                $out[$y] = 0);
            return res;
        default:
            res->len = 0;
            drop_obj(res);
            THROW_TYPE1("count", val->type);
    }

    return res;
}

obj_p aggr_count(obj_p val, obj_p index) {
    i64_t i, j, n, l;
    obj_p parts, res, filter;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDGUID:
        case TYPE_PARTEDENUM:
        case TYPE_PARTEDLIST: {
            filter = index_group_filter(index);
            l = val->len;
            res = I64(n);

            if (n == 1 && filter == NULL_OBJ) {
                // Aggregate all partitions into single result, no filter
                i64_t total = 0;
                for (i = 0; i < l; i++) {
                    total += ops_count(AS_LIST(val)[i]);
                }
                AS_I64(res)[0] = total;
            } else if (n == 1 && filter != NULL_OBJ && filter->type == TYPE_PARTEDI64) {
                // Aggregate all partitions into single result with parted filter
                i64_t total = 0;
                for (i = 0; i < l; i++) {
                    obj_p fentry = AS_LIST(filter)[i];
                    if (fentry == NULL_OBJ)
                        continue;
                    if (fentry->type == -TYPE_I64 && fentry->i64 == -1) {
                        // All rows match
                        total += ops_count(AS_LIST(val)[i]);
                    } else if (fentry->type > 0 && fentry->len > 0) {
                        // Specific rows match
                        total += fentry->len;
                    }
                }
                AS_I64(res)[0] = total;
            } else {
                for (i = 0, j = 0; i < l; i++) {
                    if (filter == NULL_OBJ || AS_LIST(filter)[i] != NULL_OBJ) {
                        obj_p fentry = (filter == NULL_OBJ) ? NULL_OBJ : AS_LIST(filter)[i];
                        if (fentry != NULL_OBJ && fentry->type == -TYPE_I64 && fentry->i64 == -1) {
                            // Take all rows from partition
                            AS_I64(res)[j++] = ops_count(AS_LIST(val)[i]);
                        } else if (fentry != NULL_OBJ && fentry->len > 0) {
                            // Take filtered rows
                            AS_I64(res)[j++] = fentry->len;
                        } else if (filter == NULL_OBJ) {
                            AS_I64(res)[j++] = ops_count(AS_LIST(val)[i]);
                        }
                    }
                }
                res->len = j;
            }
            return res;
        }
        default:
            parts = aggr_map((raw_p)aggr_count_partial, val, TYPE_I64, index);
            if (IS_ERR(parts))
                return parts;
            res = AGGR_COLLECT(parts, n, i64, i64, $out[$y] += $in[$x]);
            drop_obj(parts);
            return res;
    }
}

obj_p aggr_avg_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    // res is a list: [sums (f64), counts (i64)]
    obj_p sums_obj = AS_LIST(res)[0];
    obj_p cnts_obj = AS_LIST(res)[1];

    i64_t i, x, y, n, o;
    i64_t *group_ids, *source, *filter;
    i64_t shift;
    index_type_t index_type;
    f64_t *so;
    i64_t *co;

    index_type = index_group_type(index);
    n = (index_type == INDEX_TYPE_PARTEDCOMMON) ? 1 : index_group_count(index);
    if (index_type == INDEX_TYPE_WINDOW)
        n = len;
    o = (index_type == INDEX_TYPE_WINDOW) ? offset : 0;
    group_ids = index_group_ids(index);
    filter = index_group_filter_ids(index);

    so = AS_F64(sums_obj);
    co = AS_I64(cnts_obj);

    // Initialize
    for (y = o; y < n + o; ++y) {
        so[y] = 0.0;
        co[y] = 0;
    }

    switch (val->type) {
        case TYPE_I64: {
            i64_t *in = AS_I64(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I64) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I64) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (in[x] != NULL_I64) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (in[x] != NULL_I64) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (in[x] != NULL_I64) {
                            so[0] += (f64_t)in[x];
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (in[x] != NULL_I64) {
                                    so[y] += (f64_t)in[x];
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        case TYPE_I16: {
            i16_t *in = AS_I16(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I16) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I16) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (in[x] != NULL_I16) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (in[x] != NULL_I16) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (in[x] != NULL_I16) {
                            so[0] += (f64_t)in[x];
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (in[x] != NULL_I16) {
                                    so[y] += (f64_t)in[x];
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME: {
            i32_t *in = AS_I32(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I32) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I32) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (in[x] != NULL_I32) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (in[x] != NULL_I32) {
                                so[y] += (f64_t)in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (in[x] != NULL_I32) {
                            so[0] += (f64_t)in[x];
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (in[x] != NULL_I32) {
                                    so[y] += (f64_t)in[x];
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        case TYPE_F64: {
            f64_t *in = AS_F64(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (!ISNANF64(in[x])) {
                                so[y] += in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (!ISNANF64(in[x])) {
                                so[y] += in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (!ISNANF64(in[x])) {
                                so[y] += in[x];
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (!ISNANF64(in[x])) {
                                so[y] += in[x];
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (!ISNANF64(in[x])) {
                            so[0] += in[x];
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (!ISNANF64(in[x])) {
                                    so[y] += in[x];
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        default:
            // Clean up the list contents and return error
            drop_obj(sums_obj);
            drop_obj(cnts_obj);
            res->len = 0;
            drop_obj(res);
            THROW_TYPE1("avg", val->type);
    }
}

static obj_p aggr_map_avg_other(obj_p val, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res, sc;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    out_len = group_count;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        sc = vn_list(2, F64(out_len), I64(out_len));
        argv[4] = (raw_p)sc;
        res = pool_call_task_fn((raw_p)aggr_avg_partial, 5, argv);
        return vn_list(1, res);
    }

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++) {
        sc = vn_list(2, F64(out_len), I64(out_len));
        pool_add_task(pool, (raw_p)aggr_avg_partial, 5, chunk, i * chunk, val, index, sc);
    }

    sc = vn_list(2, F64(out_len), I64(out_len));
    pool_add_task(pool, (raw_p)aggr_avg_partial, 5, l - i * chunk, i * chunk, val, index, sc);

    return pool_run(pool);
}

static obj_p aggr_map_avg_parted(obj_p val, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res, sc, f64obj, i64obj, outer;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = val->len;
    out_len = 1;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        f64obj = F64(out_len);
        i64obj = I64(out_len);
        sc = vn_list(2, f64obj, i64obj);
        argv[4] = (raw_p)sc;
        res = pool_call_task_fn((raw_p)aggr_avg_partial, 5, argv);
        return vn_list(1, res);
    }

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++) {
        sc = vn_list(2, F64(out_len), I64(out_len));
        pool_add_task(pool, (raw_p)aggr_avg_partial, 5, chunk, i * chunk, val, index, sc);
    }

    sc = vn_list(2, F64(out_len), I64(out_len));
    pool_add_task(pool, (raw_p)aggr_avg_partial, 5, l - i * chunk, i * chunk, val, index, sc);

    return pool_run(pool);
}

static obj_p aggr_map_avg_window(obj_p val, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p v, res, sc;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    out_len = group_count;

    n = pool_get_executors_count(pool);
    res = vn_list(2, F64(out_len), I64(out_len));

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = (raw_p)res;
        res = pool_call_task_fn((raw_p)aggr_avg_partial, 5, argv);
        return vn_list(1, res);
    }

    if (n > group_count)
        n = group_count;

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++) {
        sc = vn_list(2, clone_obj(AS_LIST(res)[0]), clone_obj(AS_LIST(res)[1]));
        pool_add_task(pool, (raw_p)aggr_avg_partial, 5, chunk, i * chunk, val, index, sc);
    }

    sc = vn_list(2, clone_obj(AS_LIST(res)[0]), clone_obj(AS_LIST(res)[1]));
    pool_add_task(pool, (raw_p)aggr_avg_partial, 5, l - i * chunk, i * chunk, val, index, sc);

    v = pool_run(pool);
    if (IS_ERR(v)) {
        drop_obj(res);
        return v;
    }
    drop_obj(v);
    return vn_list(1, res);
}

static obj_p aggr_map_avg(obj_p val, obj_p index) {
    switch (index_group_type(index)) {
        case INDEX_TYPE_PARTEDCOMMON:
            return aggr_map_avg_parted(val, index);
        case INDEX_TYPE_WINDOW:
            return aggr_map_avg_window(val, index);
        default:
            return aggr_map_avg_other(val, index);
    }
}

obj_p aggr_avg(obj_p val, obj_p index) {
    i64_t i, j, l, n;
    f64_t *so, *fo;
    i64_t *co;
    obj_p parts, res, part;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_DATE:
        case TYPE_TIME:
            parts = aggr_map_avg(val, index);
            if (IS_ERR(parts))
                return parts;

            // Collect and combine partial results: parts is list of [sums, counts] lists
            l = parts->len;
            res = F64(n);
            fo = AS_F64(res);

            // Initialize from first partition
            part = AS_LIST(parts)[0];
            so = AS_F64(AS_LIST(part)[0]);
            co = AS_I64(AS_LIST(part)[1]);
            for (i = 0; i < n; i++) {
                fo[i] = so[i];
                AS_I64(AS_LIST(part)[1])[i] = co[i];  // Keep counts in first partition for accumulation
            }

            // Combine remaining partitions
            for (j = 1; j < l; j++) {
                part = AS_LIST(parts)[j];
                so = AS_F64(AS_LIST(part)[0]);
                co = AS_I64(AS_LIST(part)[1]);
                for (i = 0; i < n; i++) {
                    fo[i] += so[i];
                    AS_I64(AS_LIST(AS_LIST(parts)[0])[1])[i] += co[i];
                }
            }

            // Final division: sum / count
            co = AS_I64(AS_LIST(AS_LIST(parts)[0])[1]);
            for (i = 0; i < n; i++)
                fo[i] = (co[i] == 0) ? NULL_F64 : fo[i] / (f64_t)co[i];

            drop_obj(parts);
            return res;

        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP: {
            obj_p filter, pparts, ppart;
            f64_t total_sum;
            i64_t total_cnt;

            filter = index_group_filter(index);
            l = val->len;
            res = F64(n);
            fo = AS_F64(res);

            if (n == 1) {
                // Global aggregation (single result) - accumulate across all matching partitions
                total_sum = 0.0;
                total_cnt = 0;
                for (i = 0; i < l; i++) {
                    // Skip partitions that don't match the filter
                    if (filter != NULL_OBJ && AS_LIST(filter)[i] == NULL_OBJ)
                        continue;

                    pparts = aggr_map_avg(AS_LIST(val)[i], index);
                    if (IS_ERR(pparts)) {
                        drop_obj(res);
                        return pparts;
                    }
                    for (i64_t k = 0; k < pparts->len; k++) {
                        ppart = AS_LIST(pparts)[k];
                        total_sum += AS_F64(AS_LIST(ppart)[0])[0];
                        total_cnt += AS_I64(AS_LIST(ppart)[1])[0];
                    }
                    drop_obj(pparts);
                }
                fo[0] = (total_cnt == 0) ? NULL_F64 : total_sum / (f64_t)total_cnt;
            } else {
                // Multiple groups - one result per matching partition
                for (i = 0, j = 0; i < l; i++) {
                    if (filter == NULL_OBJ || AS_LIST(filter)[i] != NULL_OBJ) {
                        pparts = aggr_map_avg(AS_LIST(val)[i], index);
                        if (IS_ERR(pparts)) {
                            drop_obj(res);
                            return pparts;
                        }

                        // Combine all partial results for this partition
                        total_sum = 0.0;
                        total_cnt = 0;
                        for (i64_t k = 0; k < pparts->len; k++) {
                            ppart = AS_LIST(pparts)[k];
                            total_sum += AS_F64(AS_LIST(ppart)[0])[0];
                            total_cnt += AS_I64(AS_LIST(ppart)[1])[0];
                        }

                        fo[j++] = (total_cnt == 0) ? NULL_F64 : total_sum / (f64_t)total_cnt;
                        drop_obj(pparts);
                    }
                }
            }
            return res;
        }

        default:
            THROW_TYPE1("avg", val->type);
    }
}

// Partial function for median - computes median for each collected group
obj_p aggr_med_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p collected = (obj_p)arg3, res = (obj_p)arg5;
    UNUSED(arg4);

    i64_t i, l;
    i64_t *xisort;
    f64_t *xfsort;
    f64_t med;
    f64_t *fo = AS_F64(res);
    obj_p grp, sorted;

    for (i = offset; i < offset + len; i++) {
        grp = AS_LIST(collected)[i];
        l = grp->len;

        if (l == 0) {
            fo[i] = NULL_F64;
            continue;
        }

        switch (grp->type) {
            case TYPE_I64:
            case TYPE_TIMESTAMP: {
                i64_t mid = l / 2;
                sorted = ray_asc(grp);
                xisort = AS_I64(sorted);
                if (l % 2 == 0)
                    med = ((f64_t)xisort[mid - 1] + (f64_t)xisort[mid]) / 2.0;
                else
                    med = (f64_t)xisort[mid];
                drop_obj(sorted);
                fo[i] = med;
                break;
            }
            case TYPE_F64: {
                i64_t mid = l / 2;
                sorted = ray_asc(grp);
                xfsort = AS_F64(sorted);
                if (l % 2 == 0)
                    med = (xfsort[mid - 1] + xfsort[mid]) / 2.0;
                else
                    med = xfsort[mid];
                drop_obj(sorted);
                fo[i] = med;
                break;
            }
            default:
                fo[i] = NULL_F64;
                break;
        }
    }

    return res;
}

static obj_p aggr_map_med(obj_p collected, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, chunk;
    obj_p res;
    raw_p argv[6];

    n = collected->len;
    l = pool_get_executors_count(pool);

    if (l > n)
        l = n;

    res = F64(n);

    if (l == 1) {
        argv[0] = (raw_p)n;
        argv[1] = (raw_p)0;
        argv[2] = collected;
        argv[3] = index;
        argv[4] = (raw_p)res;
        res = pool_call_task_fn((raw_p)aggr_med_partial, 5, argv);
        return res;
    }

    pool_prepare(pool);
    chunk = n / l;

    for (i = 0; i < l - 1; i++)
        pool_add_task(pool, (raw_p)aggr_med_partial, 5, chunk, i * chunk, collected, index, clone_obj(res));

    pool_add_task(pool, (raw_p)aggr_med_partial, 5, n - i * chunk, i * chunk, collected, index, clone_obj(res));

    obj_p v = pool_run(pool);
    if (IS_ERR(v)) {
        drop_obj(res);
        return v;
    }
    drop_obj(v);
    return res;
}

obj_p aggr_med(obj_p val, obj_p index) {
    obj_p collected, res;

    // Collect values into groups first (this is parallelized via aggr_collect)
    collected = aggr_collect(val, index);
    if (IS_ERR(collected))
        return collected;

    // Then compute median for each group in parallel
    res = aggr_map_med(collected, index);
    drop_obj(collected);

    return res;
}

// Partial function for stddev - accumulates sum, sum_sq, and count in one pass
// Result structure: list of [sum (f64), sum_sq (f64), count (i64)]
obj_p aggr_dev_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    // res is a list: [sum (f64), sum_sq (f64), count (i64)]
    obj_p sum_obj = AS_LIST(res)[0];
    obj_p sumsq_obj = AS_LIST(res)[1];
    obj_p cnt_obj = AS_LIST(res)[2];

    i64_t i, x, y, n, o;
    i64_t *group_ids, *source, *filter;
    i64_t shift;
    index_type_t index_type;
    f64_t *so, *sqo;
    i64_t *co;
    f64_t v;

    index_type = index_group_type(index);
    n = (index_type == INDEX_TYPE_PARTEDCOMMON) ? 1 : index_group_count(index);
    if (index_type == INDEX_TYPE_WINDOW)
        n = len;
    o = (index_type == INDEX_TYPE_WINDOW) ? offset : 0;
    group_ids = index_group_ids(index);
    filter = index_group_filter_ids(index);

    so = AS_F64(sum_obj);
    sqo = AS_F64(sumsq_obj);
    co = AS_I64(cnt_obj);

    // Initialize
    for (y = o; y < n + o; ++y) {
        so[y] = 0.0;
        sqo[y] = 0.0;
        co[y] = 0;
    }

    switch (val->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP: {
            i64_t *in = AS_I64(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I64) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I64) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (in[x] != NULL_I64) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (in[x] != NULL_I64) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (in[x] != NULL_I64) {
                            v = (f64_t)in[x];
                            so[0] += v;
                            sqo[0] += v * v;
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            sqo[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (in[x] != NULL_I64) {
                                    v = (f64_t)in[x];
                                    so[y] += v;
                                    sqo[y] += v * v;
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        case TYPE_F64: {
            f64_t *in = AS_F64(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (!ISNANF64(in[x])) {
                                v = in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (!ISNANF64(in[x])) {
                                v = in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (!ISNANF64(in[x])) {
                                v = in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (!ISNANF64(in[x])) {
                                v = in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (!ISNANF64(in[x])) {
                            v = in[x];
                            so[0] += v;
                            sqo[0] += v * v;
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            sqo[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (!ISNANF64(in[x])) {
                                    v = in[x];
                                    so[y] += v;
                                    sqo[y] += v * v;
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        case TYPE_I16: {
            i16_t *in = AS_I16(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I16) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I16) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (in[x] != NULL_I16) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (in[x] != NULL_I16) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (in[x] != NULL_I16) {
                            v = (f64_t)in[x];
                            so[0] += v;
                            sqo[0] += v * v;
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            sqo[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (in[x] != NULL_I16) {
                                    v = (f64_t)in[x];
                                    so[y] += v;
                                    sqo[y] += v * v;
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME: {
            i32_t *in = AS_I32(val);

            switch (index_type) {
                case INDEX_TYPE_SHIFT:
                    source = index_group_source(index);
                    shift = index_group_shift(index);
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I32) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[source[x] - shift];
                            if (in[x] != NULL_I32) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_IDS:
                    if (filter != NULL) {
                        for (i = 0; i < len; ++i) {
                            x = filter[i + offset];
                            y = group_ids[i + offset];
                            if (in[x] != NULL_I32) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    } else {
                        for (i = 0; i < len; ++i) {
                            x = i + offset;
                            y = group_ids[x];
                            if (in[x] != NULL_I32) {
                                v = (f64_t)in[x];
                                so[y] += v;
                                sqo[y] += v * v;
                                co[y]++;
                            }
                        }
                    }
                    break;
                case INDEX_TYPE_PARTEDCOMMON:
                    for (i = 0; i < len; ++i) {
                        x = i + offset;
                        if (in[x] != NULL_I32) {
                            v = (f64_t)in[x];
                            so[0] += v;
                            sqo[0] += v * v;
                            co[0]++;
                        }
                    }
                    break;
                case INDEX_TYPE_WINDOW: {
                    i64_t li, ri, fi, ti, kl, kr, it;
                    obj_p rn;
                    it = index_group_meta(index)->i64;
                    for (i = offset; i < offset + len; ++i) {
                        y = i;
                        rn = AS_LIST(AS_LIST(index)[5])[i];
                        if (rn != NULL_OBJ) {
                            fi = AS_I64(rn)[0];
                            ti = AS_I64(rn)[1];
                            kl = AS_I32(AS_LIST(AS_LIST(index)[4])[0])[i];
                            kr = AS_I32(AS_LIST(AS_LIST(index)[4])[1])[i];
                            if (it == 0) {
                                li = indexr_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            } else {
                                li = indexl_bin_i32_(kl, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                            }
                            ri = indexr_bin_i32_(kr, AS_I32(AS_LIST(index)[3]), fi, ti - fi + 1);
                        }
                        if (rn == NULL_OBJ || AS_I32(AS_LIST(index)[3])[li] > kr ||
                            (it == 1 && AS_I32(AS_LIST(index)[3])[ri] < kl)) {
                            so[y] = NULL_F64;
                            sqo[y] = NULL_F64;
                            co[y] = 0;
                        } else {
                            for (x = li; x <= ri; ++x) {
                                if (in[x] != NULL_I32) {
                                    v = (f64_t)in[x];
                                    so[y] += v;
                                    sqo[y] += v * v;
                                    co[y]++;
                                }
                            }
                        }
                    }
                    break;
                }
            }
            return res;
        }
        default:
            // Clean up the list contents and return error
            drop_obj(sum_obj);
            drop_obj(sumsq_obj);
            drop_obj(cnt_obj);
            res->len = 0;
            drop_obj(res);
            THROW_TYPE1("dev", val->type);
    }
}

static obj_p aggr_map_dev_other(obj_p val, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res, sc;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    out_len = group_count;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        sc = vn_list(3, F64(out_len), F64(out_len), I64(out_len));
        argv[4] = (raw_p)sc;
        res = pool_call_task_fn((raw_p)aggr_dev_partial, 5, argv);
        return vn_list(1, res);
    }

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++) {
        sc = vn_list(3, F64(out_len), F64(out_len), I64(out_len));
        pool_add_task(pool, (raw_p)aggr_dev_partial, 5, chunk, i * chunk, val, index, sc);
    }

    sc = vn_list(3, F64(out_len), F64(out_len), I64(out_len));
    pool_add_task(pool, (raw_p)aggr_dev_partial, 5, l - i * chunk, i * chunk, val, index, sc);

    return pool_run(pool);
}

static obj_p aggr_map_dev_parted(obj_p val, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p res, sc;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = val->len;
    out_len = 1;

    n = pool_split_by(pool, group_len, group_count);

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        sc = vn_list(3, F64(out_len), F64(out_len), I64(out_len));
        argv[4] = (raw_p)sc;
        res = pool_call_task_fn((raw_p)aggr_dev_partial, 5, argv);
        return vn_list(1, res);
    }

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++) {
        sc = vn_list(3, F64(out_len), F64(out_len), I64(out_len));
        pool_add_task(pool, (raw_p)aggr_dev_partial, 5, chunk, i * chunk, val, index, sc);
    }

    sc = vn_list(3, F64(out_len), F64(out_len), I64(out_len));
    pool_add_task(pool, (raw_p)aggr_dev_partial, 5, l - i * chunk, i * chunk, val, index, sc);

    return pool_run(pool);
}

static obj_p aggr_map_dev_window(obj_p val, obj_p index) {
    pool_p pool = runtime_get()->pool;
    i64_t i, l, n, group_count, group_len, out_len, chunk;
    obj_p v, res, sc;
    raw_p argv[6];

    group_count = index_group_count(index);
    group_len = index_group_len(index);
    out_len = group_count;

    n = pool_get_executors_count(pool);
    res = vn_list(3, F64(out_len), F64(out_len), I64(out_len));

    if (n == 1) {
        argv[0] = (raw_p)group_len;
        argv[1] = (raw_p)0;
        argv[2] = val;
        argv[3] = index;
        argv[4] = (raw_p)res;
        res = pool_call_task_fn((raw_p)aggr_dev_partial, 5, argv);
        return vn_list(1, res);
    }

    if (n > group_count)
        n = group_count;

    pool_prepare(pool);
    l = group_len;
    chunk = l / n;

    for (i = 0; i < n - 1; i++) {
        sc = vn_list(3, clone_obj(AS_LIST(res)[0]), clone_obj(AS_LIST(res)[1]), clone_obj(AS_LIST(res)[2]));
        pool_add_task(pool, (raw_p)aggr_dev_partial, 5, chunk, i * chunk, val, index, sc);
    }

    sc = vn_list(3, clone_obj(AS_LIST(res)[0]), clone_obj(AS_LIST(res)[1]), clone_obj(AS_LIST(res)[2]));
    pool_add_task(pool, (raw_p)aggr_dev_partial, 5, l - i * chunk, i * chunk, val, index, sc);

    v = pool_run(pool);
    if (IS_ERR(v)) {
        drop_obj(res);
        return v;
    }
    drop_obj(v);
    return vn_list(1, res);
}

static obj_p aggr_map_dev(obj_p val, obj_p index) {
    switch (index_group_type(index)) {
        case INDEX_TYPE_PARTEDCOMMON:
            return aggr_map_dev_parted(val, index);
        case INDEX_TYPE_WINDOW:
            return aggr_map_dev_window(val, index);
        default:
            return aggr_map_dev_other(val, index);
    }
}

obj_p aggr_dev(obj_p val, obj_p index) {
    i64_t i, j, l, n;
    f64_t *so, *sqo, *fo;
    f64_t mean, variance;
    i64_t *co;
    obj_p parts, res, part;

    n = index_group_count(index);

    switch (val->type) {
        case TYPE_I16:
        case TYPE_I32:
        case TYPE_I64:
        case TYPE_F64:
        case TYPE_DATE:
        case TYPE_TIME:
        case TYPE_TIMESTAMP:
            parts = aggr_map_dev(val, index);
            if (IS_ERR(parts))
                return parts;

            // Collect and combine partial results: parts is list of [sum, sum_sq, count] lists
            l = parts->len;
            res = F64(n);
            fo = AS_F64(res);

            // Initialize from first partition
            part = AS_LIST(parts)[0];
            so = AS_F64(AS_LIST(part)[0]);
            sqo = AS_F64(AS_LIST(part)[1]);
            co = AS_I64(AS_LIST(part)[2]);

            // Copy first partition's values to result arrays for accumulation
            f64_t *acc_sum = AS_F64(AS_LIST(part)[0]);
            f64_t *acc_sumsq = AS_F64(AS_LIST(part)[1]);
            i64_t *acc_cnt = AS_I64(AS_LIST(part)[2]);

            // Combine remaining partitions
            for (j = 1; j < l; j++) {
                part = AS_LIST(parts)[j];
                so = AS_F64(AS_LIST(part)[0]);
                sqo = AS_F64(AS_LIST(part)[1]);
                co = AS_I64(AS_LIST(part)[2]);
                for (i = 0; i < n; i++) {
                    acc_sum[i] += so[i];
                    acc_sumsq[i] += sqo[i];
                    acc_cnt[i] += co[i];
                }
            }

            // Final computation: stddev = sqrt(sum_sq/n - (sum/n)^2)
            for (i = 0; i < n; i++) {
                if (acc_cnt[i] == 0) {
                    fo[i] = NULL_F64;
                } else if (acc_cnt[i] == 1) {
                    fo[i] = 0.0;
                } else {
                    mean = acc_sum[i] / (f64_t)acc_cnt[i];
                    variance = acc_sumsq[i] / (f64_t)acc_cnt[i] - mean * mean;
                    // Handle floating point errors that might make variance slightly negative
                    fo[i] = (variance < 0.0) ? 0.0 : sqrt(variance);
                }
            }

            drop_obj(parts);
            return res;

        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP: {
            obj_p filter, pparts, ppart;
            f64_t total_sum, total_sumsq;
            i64_t total_cnt;

            filter = index_group_filter(index);
            l = val->len;
            res = F64(n);
            fo = AS_F64(res);

            if (n == 1) {
                // Global aggregation (single result) - accumulate across all matching partitions
                total_sum = 0.0;
                total_sumsq = 0.0;
                total_cnt = 0;
                for (i = 0; i < l; i++) {
                    // Skip partitions that don't match the filter
                    if (filter != NULL_OBJ && AS_LIST(filter)[i] == NULL_OBJ)
                        continue;

                    pparts = aggr_map_dev(AS_LIST(val)[i], index);
                    if (IS_ERR(pparts)) {
                        drop_obj(res);
                        return pparts;
                    }
                    for (i64_t k = 0; k < pparts->len; k++) {
                        ppart = AS_LIST(pparts)[k];
                        total_sum += AS_F64(AS_LIST(ppart)[0])[0];
                        total_sumsq += AS_F64(AS_LIST(ppart)[1])[0];
                        total_cnt += AS_I64(AS_LIST(ppart)[2])[0];
                    }
                    drop_obj(pparts);
                }
                if (total_cnt == 0) {
                    fo[0] = NULL_F64;
                } else if (total_cnt == 1) {
                    fo[0] = 0.0;
                } else {
                    mean = total_sum / (f64_t)total_cnt;
                    variance = total_sumsq / (f64_t)total_cnt - mean * mean;
                    fo[0] = (variance < 0.0) ? 0.0 : sqrt(variance);
                }
            } else {
                // Multiple groups - one result per matching partition
                for (i = 0, j = 0; i < l; i++) {
                    if (filter == NULL_OBJ || AS_LIST(filter)[i] != NULL_OBJ) {
                        pparts = aggr_map_dev(AS_LIST(val)[i], index);
                        if (IS_ERR(pparts)) {
                            drop_obj(res);
                            return pparts;
                        }

                        // Combine all partial results for this partition
                        total_sum = 0.0;
                        total_sumsq = 0.0;
                        total_cnt = 0;
                        for (i64_t k = 0; k < pparts->len; k++) {
                            ppart = AS_LIST(pparts)[k];
                            total_sum += AS_F64(AS_LIST(ppart)[0])[0];
                            total_sumsq += AS_F64(AS_LIST(ppart)[1])[0];
                            total_cnt += AS_I64(AS_LIST(ppart)[2])[0];
                        }

                        if (total_cnt == 0) {
                            fo[j++] = NULL_F64;
                        } else if (total_cnt == 1) {
                            fo[j++] = 0.0;
                        } else {
                            mean = total_sum / (f64_t)total_cnt;
                            variance = total_sumsq / (f64_t)total_cnt - mean * mean;
                            fo[j++] = (variance < 0.0) ? 0.0 : sqrt(variance);
                        }
                        drop_obj(pparts);
                    }
                }
            }
            return res;
        }

        default:
            THROW_TYPE1("dev", val->type);
    }
}

obj_p aggr_collect(obj_p val, obj_p index) {
    i64_t i, j, l, n;
    obj_p k, v, res, filter;

    l = index_group_len(index);
    n = index_group_count(index);

    res = LIST(n);
    for (i = 0; i < n; i++)
        AS_LIST(res)[i] = vector(val->type, 0);

    switch (val->type) {
        case TYPE_B8:
            AGGR_ITER(index, l, 0, val, res, b8, list, , push_raw($out + $y, $in + $x), );
            return res;
        case TYPE_U8:
            AGGR_ITER(index, l, 0, val, res, u8, list, , push_raw($out + $y, $in + $x), );
            return res;
        case TYPE_I16:
            AGGR_ITER(index, l, 0, val, res, i16, list, , push_raw($out + $y, $in + $x), );
            return res;
        case TYPE_I32:
        case TYPE_DATE:
        case TYPE_TIME:
            AGGR_ITER(index, l, 0, val, res, i32, list, , push_raw($out + $y, $in + $x), );
            return res;
        case TYPE_I64:
        case TYPE_SYMBOL:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, l, 0, val, res, i64, list, , push_raw($out + $y, $in + $x), );
            return res;
        case TYPE_F64:
            AGGR_ITER(index, l, 0, val, res, f64, list, , push_raw($out + $y, $in + $x), );
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
                drop_obj(v);
                drop_obj(res);
                return ray_error(ERR_TYPE, "enum: '%s' is not a 'Symbol'", type_name(v->type));
            }

            AGGR_ITER(index, l, 0, val, res, i64, list, , push_raw($out + $y, AS_SYMBOL(v) + $in[$x]), );
            drop_obj(v);
            return res;
        case TYPE_GUID:
            AGGR_ITER(index, l, 0, val, res, guid, list, , push_raw($out + $y, $in + $x), );
            return res;
        case TYPE_LIST:
            AGGR_ITER(index, l, 0, val, res, list, list, , push_obj($out + $y, clone_obj($in[$x])), );
            return res;
        case TYPE_PARTEDB8:
        case TYPE_PARTEDU8:
        case TYPE_PARTEDI16:
        case TYPE_PARTEDI32:
        case TYPE_PARTEDI64:
        case TYPE_PARTEDF64:
        case TYPE_PARTEDDATE:
        case TYPE_PARTEDTIME:
        case TYPE_PARTEDTIMESTAMP:
        case TYPE_PARTEDGUID:
        case TYPE_PARTEDENUM:
        case TYPE_PARTEDLIST:
            // For parted types with INDEX_TYPE_PARTEDCOMMON, each partition is a group
            // Collect all values from matching partitions
            drop_obj(res);
            filter = index_group_filter(index);
            res = LIST(n);
            if (filter == NULL_OBJ) {
                // No filter - each partition is a group
                for (i = 0; i < (i64_t)val->len; i++)
                    AS_LIST(res)[i] = ray_value(AS_LIST(val)[i]);
            } else {
                // With filter - only include matching partitions
                l = filter->len;
                for (i = 0, j = 0; i < l; i++) {
                    if (AS_LIST(filter)[i] != NULL_OBJ)
                        AS_LIST(res)[j++] = ray_value(AS_LIST(val)[i]);
                }
                res->len = j;
            }
            return res;
        default:
            drop_obj(res);
            THROW_TYPE1("collect", val->type);
    }
}

obj_p aggr_row(obj_p val, obj_p index) {
    i64_t i, l, n;
    obj_p res;

    l = index_group_len(index);
    n = index_group_count(index);

    res = LIST(n);
    for (i = 0; i < n; i++)
        AS_LIST(res)[i] = I64(0);

    AGGR_ITER(
        index, l, 0, val, res, i64, list, , {
            UNUSED($in);
            push_raw($out + $y, &$x);
        }, );

    return res;
}
