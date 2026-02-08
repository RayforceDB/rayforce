/*
 *   Copyright (c) 2025 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#include "../core/dynlib.h"
#include "../core/heap.h"

static i32_t dropped_val = 0;

nil_t drop_ext(raw_p ptr) {
    i32_t* val = (i32_t*)ptr;
    dropped_val = *val;
    heap_free(val);
}

test_result_t test_external() {
    obj_p o, c;
    i32_t* val;
    ext_p ext;

    dropped_val = 0;

    val = heap_alloc(sizeof(i32_t));
    *val = 42;

    o = external((raw_p)val, drop_ext);

    if (o->type != TYPE_EXT) {
        char msg[100];
        sprintf(msg, "Type must be TYPE_EXT (%d), got %d", TYPE_EXT, o->type);
        FAIL(strdup(msg));
    }
    TEST_ASSERT(rc_obj(o) == 1, "Refcount must be 1");

    // Check if the external pointer is correctly stored
    // Note: AS_C8(o) returns pointer to payload after obj_t header
    // So we cast that payload to ext_p
    ext = (ext_p)AS_C8(o);
    TEST_ASSERT(ext->ptr == val, "External pointer mismatch");

    c = clone_obj(o);
    TEST_ASSERT(rc_obj(o) == 2, "Refcount must be 2 after clone");

    drop_obj(c);
    TEST_ASSERT(rc_obj(o) == 1, "Refcount must be 1 after drop");
    TEST_ASSERT(dropped_val == 0, "Drop handler must not be called yet");

    drop_obj(o);
    TEST_ASSERT(dropped_val == 42, "Drop handler must be called");

    PASS();
}
