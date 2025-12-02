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

obj_p filter_collect(obj_p val, obj_p index) { return at_ids(val, AS_I64(index), index->len); }
