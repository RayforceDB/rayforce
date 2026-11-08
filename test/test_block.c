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
#include "mem/heap.h"
#include "table/sym.h"
#include "ops/ops.h"

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

/* ---- ray_block_size: RAY_LIST branch ------------------------------------ */

static test_result_t test_block_size_list(void) {
    ray_t list;
    memset(&list, 0, sizeof(list));
    list.type = RAY_LIST;
    list.len  = 3;

    size_t sz = ray_block_size(&list);
    /* 32 header + 3 * sizeof(ray_t*) = 32 + 24 = 56 */
    TEST_ASSERT_EQ_U(sz, 32 + (size_t)3 * sizeof(ray_t*));

    /* Empty list: still goes through the LIST branch */
    list.len = 0;
    TEST_ASSERT_EQ_U(ray_block_size(&list), 32);

    PASS();
}

/* ---- ray_block_size: RAY_DICT branch ------------------------------------ */

static test_result_t test_block_size_dict(void) {
    ray_t d;
    memset(&d, 0, sizeof(d));
    d.type = RAY_DICT;
    d.len  = 2;

    size_t sz = ray_block_size(&d);
    /* 32 header + 2 * sizeof(ray_t*) = 32 + 16 = 48 */
    TEST_ASSERT_EQ_U(sz, 32 + 2 * sizeof(ray_t*));

    PASS();
}

/* ---- ray_block_size: RAY_SEL branch ------------------------------------- */

static test_result_t test_block_size_sel(void) {
    /* Use ray_sel_new to get a properly-typed block, then measure it. */
    ray_t* sel = ray_sel_new(1024);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_FMT(!RAY_IS_ERR(sel), "ray_sel_new failed");

    size_t sz = ray_block_size(sel);
    /* nrows=1024: n_segs=1, n_words=16
     * dsz = sizeof(ray_sel_meta_t)=16
     *      + align8(1)=8          (seg_flags)
     *      + align8(2)=8          (seg_popcnt)
     *      + 16*8=128             (bits)
     *      = 160
     * total = 32 + 160 = 192 */
    TEST_ASSERT_EQ_U(sz, 192);

    ray_free(sel);
    PASS();
}

static test_result_t test_block_size_sel_zero(void) {
    /* nrows=0: n_segs=0, n_words=0
     * dsz = sizeof(ray_sel_meta_t)=16 + 0 + 0 + 0 = 16
     * total = 32 + 16 = 48 */
    ray_t* sel = ray_sel_new(0);
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_FMT(!RAY_IS_ERR(sel), "ray_sel_new(0) failed");

    size_t sz = ray_block_size(sel);
    TEST_ASSERT_EQ_U(sz, 32 + sizeof(ray_sel_meta_t));

    ray_free(sel);
    PASS();
}

static test_result_t test_block_size_sel_negative(void) {
    /* nrows < 0: defensive path — returns 32 */
    ray_t fake_sel;
    memset(&fake_sel, 0, sizeof(fake_sel));
    fake_sel.type = RAY_SEL;
    fake_sel.len  = -1;   /* negative */

    size_t sz = ray_block_size(&fake_sel);
    TEST_ASSERT_EQ_U(sz, 32);

    PASS();
}

/* ---- ray_block_size: out-of-range type guard ---------------------------- */

static test_result_t test_block_size_bad_type(void) {
    /* type=0 is RAY_LIST, handled above; type < 0 is atom, handled above.
     * type >= RAY_TYPE_COUNT is out-of-range for a non-atom, non-special block. */
    ray_t v;
    memset(&v, 0, sizeof(v));
    v.type = RAY_TYPE_COUNT;   /* == 15, out-of-range */
    v.len  = 10;

    size_t sz = ray_block_size(&v);
    TEST_ASSERT_EQ_U(sz, 32);

    PASS();
}

/* ---- ray_block_copy: LIST and SEL --------------------------------------- */

static test_result_t test_block_copy_list(void) {
    /* Allocate a small list, copy it, verify independence */
    ray_t* src = ray_list_new(2);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_FMT(!RAY_IS_ERR(src), "ray_list_new failed");

    ray_t* dst = ray_block_copy(src);
    TEST_ASSERT_NOT_NULL(dst);
    TEST_ASSERT_FMT(!RAY_IS_ERR(dst), "ray_block_copy failed");

    TEST_ASSERT_EQ_I(dst->type, src->type);
    TEST_ASSERT_EQ_I(dst->len,  src->len);

    ray_release(dst);
    ray_release(src);
    PASS();
}

static test_result_t test_block_copy_sel(void) {
    ray_t* src = ray_sel_new(64);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_FMT(!RAY_IS_ERR(src), "ray_sel_new failed");

    ray_t* dst = ray_block_copy(src);
    TEST_ASSERT_NOT_NULL(dst);
    TEST_ASSERT_FMT(!RAY_IS_ERR(dst), "ray_block_copy(sel) failed");

    TEST_ASSERT_EQ_I(dst->type, RAY_SEL);
    TEST_ASSERT_EQ_I(dst->len,  src->len);
    TEST_ASSERT_EQ_U(ray_block_size(dst), ray_block_size(src));

    ray_free(dst);
    ray_free(src);
    PASS();
}

/* ---- Suite definition -------------------------------------------------- */

/* The copy tests allocate: own a heap for the test and destroy it after, so
 * no heap (and the environment it resolved) outlives the test on this thread. */
static void block_setup(void)    { ray_heap_init(); }
static void block_teardown(void) { ray_heap_destroy(); }

const test_entry_t block_entries[] = {
    { "block/type_macros", test_type_macros, NULL, NULL },
    { "block/ray_data", test_ray_data, NULL, NULL },
    { "block/elem_size", test_elem_size, NULL, NULL },
    { "block/block_size_atom", test_block_size_atom, NULL, NULL },
    { "block/block_size_vec", test_block_size_vec, NULL, NULL },
    { "block/block_size_bool", test_block_size_vec_bool, NULL, NULL },
    { "block/block_size_empty", test_block_size_empty_vec, NULL, NULL },
    { "block/ray_t_size", test_ray_t_size, NULL, NULL },
    { "block/block_size_list", test_block_size_list, NULL, NULL },
    { "block/block_size_dict", test_block_size_dict, NULL, NULL },
    { "block/block_size_sel", test_block_size_sel, NULL, NULL },
    { "block/block_size_sel_zero", test_block_size_sel_zero, NULL, NULL },
    { "block/block_size_sel_negative", test_block_size_sel_negative, NULL, NULL },
    { "block/block_size_bad_type", test_block_size_bad_type, NULL, NULL },
    { "block/block_copy_list", test_block_copy_list, block_setup, block_teardown },
    { "block/block_copy_sel", test_block_copy_sel, block_setup, block_teardown },
    { NULL, NULL, NULL, NULL },
};


