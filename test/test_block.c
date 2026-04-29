/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "core/block.h"
#include "table/sym.h"

/* ---- Accessor macro tests ---------------------------------------------- */

static test_result_t test_type_macros(void) {
    ray_t atom;
    memset(&atom, 0, sizeof(atom));
    atom.type = -RAY_I64;  /* atom */
    TEST_ASSERT_EQ_I(ray_type(&atom), -RAY_I64);
    TEST_ASSERT_TRUE(ray_is_atom(&atom));
    TEST_ASSERT_FALSE(ray_is_vec(&atom));

    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_F64;    /* vector */
    vec.len  = 100;
    TEST_ASSERT_EQ_I(ray_type(&vec), RAY_F64);
    TEST_ASSERT_FALSE(ray_is_atom(&vec));
    TEST_ASSERT_TRUE(ray_is_vec(&vec));
    TEST_ASSERT_EQ_I(ray_len(&vec), 100);

    ray_t list;
    memset(&list, 0, sizeof(list));
    list.type = RAY_LIST;  /* neither atom nor vec */
    TEST_ASSERT_FALSE(ray_is_atom(&list));
    TEST_ASSERT_FALSE(ray_is_vec(&list));

    PASS();
}

static test_result_t test_ray_data(void) {
    ray_t block;
    memset(&block, 0, sizeof(block));
    void* data = ray_data(&block);
    /* Data should be exactly 32 bytes past the start of the block */
    TEST_ASSERT_EQ_I((char*)data - (char*)&block, 32);

    PASS();
}

static test_result_t test_elem_size(void) {
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_BOOL), 1);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_U8), 1);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_I16), 2);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_I32), 4);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_I64), 8);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_F64), 8);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_SYM), 8);  /* W64 default */
    TEST_ASSERT_EQ_I(ray_sym_elem_size(RAY_SYM, RAY_SYM_W8), 1);
    TEST_ASSERT_EQ_I(ray_sym_elem_size(RAY_SYM, RAY_SYM_W16), 2);
    TEST_ASSERT_EQ_I(ray_sym_elem_size(RAY_SYM, RAY_SYM_W32), 4);
    TEST_ASSERT_EQ_I(ray_sym_elem_size(RAY_SYM, RAY_SYM_W64), 8);
    TEST_ASSERT_EQ_I(ray_elem_size(RAY_GUID), 16);

    PASS();
}

/* ---- ray_block_size tests ----------------------------------------------- */

static test_result_t test_block_size_atom(void) {
    ray_t atom;
    memset(&atom, 0, sizeof(atom));
    atom.type = -RAY_F64;  /* atom */
    atom.f64  = 3.14;

    size_t sz = ray_block_size(&atom);
    TEST_ASSERT_EQ_U(sz, 32);

    PASS();
}

static test_result_t test_block_size_vec(void) {
    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_I64;
    vec.len  = 10;

    size_t sz = ray_block_size(&vec);
    /* 32 header + 10 * 8 bytes = 112 */
    TEST_ASSERT_EQ_U(sz, 112);

    PASS();
}

static test_result_t test_block_size_vec_bool(void) {
    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_BOOL;
    vec.len  = 1024;

    size_t sz = ray_block_size(&vec);
    /* 32 header + 1024 * 1 = 1056 */
    TEST_ASSERT_EQ_U(sz, 1056);

    PASS();
}

static test_result_t test_block_size_empty_vec(void) {
    ray_t vec;
    memset(&vec, 0, sizeof(vec));
    vec.type = RAY_F64;
    vec.len  = 0;

    size_t sz = ray_block_size(&vec);
    TEST_ASSERT_EQ_U(sz, 32);

    PASS();
}

/* ---- ray_t struct size check -------------------------------------------- */

static test_result_t test_ray_t_size(void) {
    /* ray_t must be exactly 32 bytes */
    TEST_ASSERT_EQ_U(sizeof(ray_t), 32);

    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

const test_entry_t block_entries[] = {
    { "block/type_macros", test_type_macros, NULL, NULL },
    { "block/ray_data", test_ray_data, NULL, NULL },
    { "block/elem_size", test_elem_size, NULL, NULL },
    { "block/block_size_atom", test_block_size_atom, NULL, NULL },
    { "block/block_size_vec", test_block_size_vec, NULL, NULL },
    { "block/block_size_bool", test_block_size_vec_bool, NULL, NULL },
    { "block/block_size_empty", test_block_size_empty_vec, NULL, NULL },
    { "block/ray_t_size", test_ray_t_size, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


