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

#include "group.h"
#include "error.h"
#include "ops.h"
#include "util.h"
#include "index.h"
#include "aggr.h"
#include "items.h"
#include "unary.h"
#include "eval.h"
#include "string.h"
#include "hash.h"
#include "pool.h"

obj_p group_map(obj_p val, obj_p index)
{
    u64_t i, l;
    obj_p v, res;

    switch (val->type)
    {
    case TYPE_TABLE:
        l = as_list(val)[1]->len;
        res = list(l);
        for (i = 0; i < l; i++)
        {
            v = as_list(as_list(val)[1])[i];
            as_list(res)[i] = group_map(v, index);
        }

        return table(clone_obj(as_list(val)[0]), res);

    default:
        res = vn_list(2, clone_obj(val), clone_obj(index));
        res->type = TYPE_GROUPMAP;
        return res;
    }
}

obj_p build_partitions(u64_t partitions, i64_t keys[], u64_t len, hash_f hash, cmp_f cmp)
{
    u64_t i, j, l;
    i64_t idx, *outkeys;
    obj_p morsels, *morsel;

    morsels = list(partitions);
    l = len / partitions;

    for (i = 0; i < partitions; i++)
        as_list(morsels)[i] = ht_oa_create(l, -1);

    for (i = 0; i < len; i++)
    {
        j = keys[i] % partitions;
        morsel = as_list(morsels) + j;
        idx = ht_oa_tab_next(morsel, keys[i]);
        outkeys = as_i64(as_list(*morsel)[0]);
        if (outkeys[idx] == NULL_I64)
            outkeys[idx] = keys[i];
    }

    return morsels;
}

obj_p aggregate_partitions(obj_p partitions, u64_t partition_idx, u64_t len, hash_f hash, cmp_f cmp, u64_t *groups)
{
    u64_t i, j, n, l, count;
    i64_t *inkeys, *outkeys, idx;
    obj_p partition, morsel;

    partition = ht_oa_create(len, -1);
    l = partitions->len;
    count = 0;

    // take morsel from every partition that belows to our idx and merge them into one partition
    for (i = 0; i < l; i++)
    {
        morsel = as_list(as_list(partitions)[i])[partition_idx];
        n = as_list(morsel)[0]->len;
        inkeys = as_i64(as_list(morsel)[0]);
        for (j = 0; j < n; j++)
        {
            if (inkeys[j] == NULL_I64)
                continue;

            idx = ht_oa_tab_next(&partition, inkeys[j]);
            outkeys = as_i64(as_list(partition)[0]);
            if (outkeys[idx] == NULL_I64)
            {
                as_i64(as_list(partition)[0])[idx] = inkeys[j];
                count++;
            }
        }
    }

    return partition;
}

u64_t group_build_index(i64_t keys[], u64_t len, hash_f hash, cmp_f cmp)
{
    u64_t i, l, groups;
    obj_p morsels, partitions;
    pool_p pool;

    groups = 0;
    pool = pool_get();
    l = pool_split_by(pool, len, 0);

    // build morsels for every partition
    pool_prepare(pool);
    for (i = 0; i < l - 1; i++)
        pool_add_task(pool, build_partitions, 5, l, keys + i * (len / l), len / l, hash, cmp);

    pool_add_task(pool, build_partitions, 5, l, keys + i * (len / l), len - (i * (len / l)), hash, cmp);
    morsels = pool_run(pool);

    timeit_tick("build partitions");

    // merge morsels into partitions
    pool_prepare(pool);
    for (i = 0; i < l - 1; i++)
        pool_add_task(pool, aggregate_partitions, 5, morsels, i, len / l, hash, cmp);

    pool_add_task(pool, aggregate_partitions, 5, morsels, i, len - (i * (len / l)), hash, cmp);
    partitions = pool_run(pool);

    timeit_tick("aggregate partitions");

    drop_obj(morsels);
    drop_obj(partitions);

    timeit_tick("drop morsels and partitions");

    return groups;
}