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

#include "fdmap.h"
#include "fs.h"
#include "mmap.h"
#include "ops.h"
#include "util.h"

obj_p fdmap_create() {
    obj_p fdmap;

    fdmap = LIST(1);
    fdmap->type = TYPE_MAPFD;

    return fdmap;
}

nil_t fdmap_add_fd(obj_p *fdmap, obj_p obj, i64_t fd, i64_t size) {
    obj_p v;

    v = I64(3);
    AS_I64(v)[0] = (i64_t)obj;
    AS_I64(v)[1] = fd;
    AS_I64(v)[2] = size;
    AS_LIST(*fdmap)[0] = v;
    // push_obj(fdmap, v);
}

nil_t fdmap_destroy(obj_p fdmap) {
    u64_t i, l, size;
    i64_t fd;
    raw_p obj;

    l = fdmap->len;

    for (i = 0; i < l; i++) {
        obj = (raw_p)AS_I64(AS_LIST(fdmap)[i])[0];
        fd = (u64_t)AS_I64(AS_LIST(fdmap)[i])[1];
        size = AS_I64(AS_LIST(fdmap)[i])[2];

        if (obj != NULL)
            mmap_free(obj, size);

        if (fd != -1)
            fs_fclose(fd);

        drop_obj(AS_LIST(fdmap)[i]);
    }
}
