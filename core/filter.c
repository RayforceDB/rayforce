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

#include "filter.h"
#include "compose.h"
#include "ops.h"
#include "items.h"

obj_p filter_map(obj_p val, obj_p index) {
    i64_t i, l;
    obj_p v, res;

    switch (val->type) {
        case TYPE_TABLE:
            l = AS_LIST(val)[1]->len;
            res = LIST(l);
            for (i = 0; i < l; i++) {
                v = AS_LIST(AS_LIST(val)[1])[i];
                AS_LIST(res)[i] = filter_map(v, index);
            }

            return table(clone_obj(AS_LIST(val)[0]), res);

        default:
            res = vn_list(2, clone_obj(val), clone_obj(index));
            res->type = TYPE_MAPFILTER;
            return res;
    }
}

obj_p filter_collect(obj_p val, obj_p index) {
    i64_t i, l, n, total;
    obj_p idx, v, res, parts;

    // Handle parted indices
    if (index->type == TYPE_PARTEDI64) {
        l = index->len;

        // Handle TYPE_MAPCOMMON (virtual column like Date)
        // Structure: AS_LIST(val)[0] = values, AS_LIST(val)[1] = counts per partition
        if (val->type == TYPE_MAPCOMMON) {
            // Count total matching rows
            total = 0;
            for (i = 0; i < l; i++) {
                idx = AS_LIST(index)[i];
                if (idx != NULL_OBJ) {
                    if (idx->type == -TYPE_I64 && idx->i64 == -1) {
                        // Take all rows from partition i
                        total += AS_I64(AS_LIST(val)[1])[i];
                    } else if (idx->len > 0) {
                        // Take specific rows
                        total += idx->len;
                    }
                }
            }

            // Create result with appropriate type
            res = vector(AS_LIST(val)[0]->type, total);
            n = 0;
            for (i = 0; i < l; i++) {
                idx = AS_LIST(index)[i];
                if (idx != NULL_OBJ) {
                    i64_t count, j;
                    if (idx->type == -TYPE_I64 && idx->i64 == -1) {
                        count = AS_I64(AS_LIST(val)[1])[i];
                    } else if (idx->len > 0) {
                        count = idx->len;
                    } else {
                        continue;
                    }
                    // Fill with the partition value
                    for (j = 0; j < count; j++) {
                        switch (AS_LIST(val)[0]->type) {
                            case TYPE_DATE:
                            case TYPE_I32:
                                AS_I32(res)[n + j] = AS_I32(AS_LIST(val)[0])[i];
                                break;
                            case TYPE_I64:
                            case TYPE_TIMESTAMP:
                                AS_I64(res)[n + j] = AS_I64(AS_LIST(val)[0])[i];
                                break;
                            default:
                                // Generic copy for other types
                                AS_I64(res)[n + j] = AS_I64(AS_LIST(val)[0])[i];
                                break;
                        }
                    }
                    n += count;
                }
            }
            return res;
        }

        // Handle parted types (TYPE_PARTED*)
        // Count matching partitions first to avoid NULL_OBJ entries
        n = 0;
        for (i = 0; i < l; i++) {
            idx = AS_LIST(index)[i];
            if (idx != NULL_OBJ && !(idx->type == -TYPE_I64 && idx->i64 == -1 && ops_count(AS_LIST(val)[i]) == 0)) {
                if (idx->type == -TYPE_I64 && idx->i64 == -1) {
                    n++;  // Marker -1: take all rows from partition
                } else if (idx->len > 0) {
                    n++;  // Has indices to select
                }
            }
        }

        parts = LIST(n);
        n = 0;

        for (i = 0; i < l; i++) {
            idx = AS_LIST(index)[i];
            if (idx == NULL_OBJ) {
                // Partition doesn't match at all - skip
                continue;
            } else if (idx->type == -TYPE_I64 && idx->i64 == -1) {
                // Marker -1 means "take all rows from this partition"
                v = AS_LIST(val)[i];
                res = ray_value(v);
                if (res != NULL_OBJ && ops_count(res) > 0) {
                    AS_LIST(parts)[n++] = res;
                } else if (res != NULL_OBJ) {
                    drop_obj(res);
                }
            } else if (idx->len == 0) {
                // Empty index list - skip
                continue;
            } else {
                // Use index list to select specific rows
                v = AS_LIST(val)[i];
                res = at_ids(v, AS_I64(idx), idx->len);
                if (res != NULL_OBJ && !IS_ERR(res)) {
                    AS_LIST(parts)[n++] = res;
                }
            }
        }

        parts->len = n;
        res = ray_raze(parts);
        drop_obj(parts);
        return res;
    }

    return at_ids(val, AS_I64(index), index->len);
}
