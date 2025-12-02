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
#include "rayforce.h"
#include "util.h"
#include "items.h"
#include "unary.h"
#include "string.h"
#include "runtime.h"
#include "index.h"
#include "pool.h"

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
            return ray_error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
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
                    return ray_error(ERR_TYPE, "first: can not resolve an enum");
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
            return ray_error(ERR_TYPE, "first: unsupported type: '%s'", type_name(val->type));
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
            return ray_error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
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
                    return ray_error(ERR_TYPE, "first: can not resolve an enum");
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
            return ray_error(ERR_TYPE, "last: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_sum_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = 0, $out[$y] = ADDI64($out[$y], $in[$x]),
                      $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = 0.0, $out[$y] = ADDF64($out[$y], $in[$x]),
                      $out[$y] = NULL_F64);
            return res;
        default:
            destroy_partial_result(res);
            return ray_error(ERR_TYPE, "sum partial: unsupported type: '%s'", type_name(val->type));
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
            return ray_error(ERR_TYPE, "sum: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_max_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = NULL_I64, $out[$y] = MAXI64($out[$y], $in[$x]),
                      $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = NULL_F64, $out[$y] = MAXF64($out[$y], $in[$x]),
                      $out[$y] = NULL_F64);
            return res;
        default:
            destroy_partial_result(res);
            return ray_error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
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
            return ray_error(ERR_TYPE, "max: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min_partial(raw_p arg1, raw_p arg2, raw_p arg3, raw_p arg4, raw_p arg5) {
    i64_t len = (i64_t)arg1, offset = (i64_t)arg2;
    obj_p val = (obj_p)arg3, index = (obj_p)arg4, res = (obj_p)arg5;

    switch (val->type) {
        case TYPE_I64:
        case TYPE_TIMESTAMP:
            AGGR_ITER(index, len, offset, val, res, i64, i64, $out[$y] = INF_I64, $out[$y] = MINI64($out[$y], $in[$x]),
                      $out[$y] = NULL_I64);
            return res;
        case TYPE_F64:
            AGGR_ITER(index, len, offset, val, res, f64, f64, $out[$y] = INF_F64, $out[$y] = MINF64($out[$y], $in[$x]),
                      $out[$y] = NULL_F64);
            return res;
        default:
            THROW(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
    }
}

obj_p aggr_min(obj_p val, obj_p index) {
    i64_t n;
    obj_p parts, res;

    n = index_group_count(index);
    parts = aggr_map((raw_p)aggr_min_partial, val, val->type, index);
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
            return ray_error(ERR_TYPE, "min: unsupported type: '%s'", type_name(val->type));
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
            return ray_error(ERR_TYPE, "count: unsupported type: '%s'", type_name(val->type));
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
            return ray_error(ERR_TYPE, "avg: unsupported type: '%s'", type_name(val->type));
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
    i64_t i, l, n;
    obj_p k, v, res;

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
        default:
            drop_obj(res);
            THROW(ERR_TYPE, "collect: unsupported type: '%s", type_name(val->type));
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
