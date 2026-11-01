#include "test.h"
#include "lang/read_budget.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "mem/arena.h"
#include "table/sym.h"
#include "core/platform.h"
#include <stdatomic.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static void setup(void) { ray_heap_init(); (void)ray_sym_init(); }
static void teardown(void) { ray_sym_destroy(); ray_heap_direct_cache_drain(); ray_heap_destroy(); }

static test_result_t budget_edges(void) {
    size_t charge;
    TEST_ASSERT_TRUE(ray_alloc_charge(80, &charge));
    TEST_ASSERT_EQ_U(charge, 128); /* Includes the header once. */
    for (int delta = -1; delta <= 1; delta++) {
        ray_read_budget_t b;
        TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
        ray_t* warm = ray_read_alloc(&b, 0);
        TEST_ASSERT_NOT_NULL(warm);
        ray_read_release(&b, warm);
        size_t base = b.live;
        b.limit = base + charge + delta;
        ray_t* v = ray_read_alloc(&b, 80);
        if (delta < 0) {
            TEST_ASSERT_NULL(v);
            TEST_ASSERT_EQ_U(b.live, base);
            TEST_ASSERT_EQ_I(b.status, RAY_READ_LIMIT);
        } else {
            TEST_ASSERT_NOT_NULL(v);
            TEST_ASSERT_EQ_U(b.live, base + charge);
        }
        TEST_ASSERT(b.peak <= b.limit || b.peak == base, "no budget overrun");
        ray_read_budget_close(&b);
        TEST_ASSERT_EQ_U(b.live, 0);
    }
    TEST_ASSERT_FALSE(ray_alloc_charge(SIZE_MAX, &charge));
    TEST_ASSERT_FALSE(ray_sys_charge(SIZE_MAX, &charge));
    PASS();
}

static test_result_t transitive_ownership(void) {
    ray_read_budget_t b;
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
    const int64_t values[] = {9007199254740993LL, INT64_MIN};
    ray_t* s = ray_read_string(&b, "long string with bytes", 22);
    ray_t* v = ray_read_i64(&b, values, 2);
    ray_t* items[] = {s, v};
    ray_t* root = ray_read_list(&b, items, 2);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_TRUE((v->attrs & RAY_ATTR_HAS_NULLS) != 0);
    ray_retain(root); /* Real heap values survive context ownership transfer. */
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);
    TEST_ASSERT_EQ_I(root->type, RAY_LIST);
    ray_t** children = ray_data(root);
    TEST_ASSERT_EQ_U(ray_str_len(children[0]), 22);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(children[1]))[0], 9007199254740993LL);
    ray_release(root);
    PASS();
}

static test_result_t construction_faults(void) {
    bool reached_success = false;
    for (size_t fail = 0; fail < 16; fail++) {
        ray_read_budget_t b;
        TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
        b.fail_after = fail;
        ray_t* s = ray_read_string(&b, "non-inline-string", 17);
        if (s) reached_success = true;
        else TEST_ASSERT_EQ_I(b.status, RAY_READ_OOM);
        TEST_ASSERT(b.live <= b.limit, "fault did not exceed admission");
        ray_read_budget_close(&b);
        TEST_ASSERT_EQ_U(b.live, 0);
        if (reached_success) break;
    }
    TEST_ASSERT_TRUE(reached_success);
    PASS();
}

static test_result_t resize_peak(void) {
    ray_read_budget_t b;
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
    unsigned char* old = ray_read_sys_alloc(&b, 128);
    TEST_ASSERT_NOT_NULL(old);
    old[0] = 77;
    size_t charge;
    TEST_ASSERT_TRUE(ray_sys_charge(8192, &charge));
    size_t before = b.live;
    b.limit = before + charge - 1;
    TEST_ASSERT_NULL(ray_read_sys_resize(&b, old, 128, 8192));
    TEST_ASSERT_EQ_U(old[0], 77);
    TEST_ASSERT_EQ_U(b.live, before);
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);

    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
    old = ray_read_sys_alloc(&b, 128);
    TEST_ASSERT_NOT_NULL(old);
    old[0] = 88;
    before = b.live;
    unsigned char* next = ray_read_sys_resize(&b, old, 128, 8192);
    TEST_ASSERT_NOT_NULL(next);
    TEST_ASSERT_EQ_U(next[0], 88);
    TEST_ASSERT_EQ_U(b.peak, before + charge);
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);
    PASS();
}

static test_result_t oversized_cache(void) {
    ray_heap_direct_cache_drain();
    int64_t old_watermark = ray_heap_anon_watermark();
    ray_heap_set_anon_watermark(ray_heap_anon_committed() + ((int64_t)1 << 30));
    size_t need = 17u << 20, larger = 19u << 20, charge;
    ray_t* big = ray_alloc(larger);
    TEST_ASSERT_NOT_NULL(big);
    big->type = RAY_U8; big->len = 0;
    size_t big_charge = ray_direct_map_size(big);
    ray_release(big);
    TEST_ASSERT_TRUE(ray_alloc_charge(need, &charge));
    ray_t* limited = ray_alloc_admitted(need, charge, &charge);
    TEST_ASSERT_NOT_NULL(limited);
    limited->type = RAY_U8; limited->len = 0;
    TEST_ASSERT_EQ_U(ray_direct_map_size(limited), charge);
    TEST_ASSERT(charge < big_charge, "reader did not take the larger cached block");
    /* Ordinary allocation still permits oversized reuse. */
    ray_t* ordinary = ray_alloc(need);
    TEST_ASSERT_NOT_NULL(ordinary);
    ordinary->type = RAY_U8; ordinary->len = 0;
    TEST_ASSERT_EQ_U(ray_direct_map_size(ordinary), big_charge);
    ray_release(ordinary); ray_release(limited);
    ray_heap_direct_cache_drain();
    ray_heap_set_anon_watermark(old_watermark);
    PASS();
}

/* A ledger reference may be dropped only when it really releases the block.
 * Parent LISTs keep their children alive even if a caller discards its handle. */
static test_result_t retained_child_admission(void) {
    const int64_t values[1024] = {0};
    for (size_t aliases = 1; aliases <= 2; aliases++) {
        ray_read_budget_t b;
        TEST_ASSERT_TRUE(ray_read_budget_init(&b, 32768));
        ray_t* child = ray_read_i64(&b, values, 1024);
        TEST_ASSERT_NOT_NULL(child);
        ray_t* children[] = {child};
        for (size_t i = 0; i < aliases; i++)
            TEST_ASSERT_NOT_NULL(ray_read_list(&b, children, 1));
        size_t before = b.live;
        ray_read_release(&b, child);
        TEST_ASSERT_EQ_U(b.live, before);
        TEST_ASSERT_NULL(ray_read_i64(&b, values, 1024));
        TEST_ASSERT_EQ_I(b.status, RAY_READ_LIMIT);
        TEST_ASSERT(b.peak <= b.limit, "no uncharged retained child");
        ray_read_budget_close(&b);
        TEST_ASSERT_EQ_U(b.live, 0);
        TEST_ASSERT_EQ_I(b.status, RAY_READ_LIMIT);
        TEST_ASSERT_TRUE(b.closed);
    }
    PASS();
}

static test_result_t retained_child_reclaim(void) {
    const int64_t values[1024] = {0};
    ray_read_budget_t b;
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 32768));
    ray_t* child = ray_read_i64(&b, values, 1024);
    TEST_ASSERT_NOT_NULL(child);
    ray_t* children[] = {child};
    ray_t* a = ray_read_list(&b, children, 1);
    ray_t* c = ray_read_list(&b, children, 1);
    TEST_ASSERT_NOT_NULL(a); TEST_ASSERT_NOT_NULL(c);
    ray_read_release(&b, child);
    ray_read_release(&b, a);
    size_t shared = b.live;
    ray_read_release(&b, child);
    TEST_ASSERT_EQ_U(b.live, shared);
    ray_read_release(&b, c);
    size_t unique = b.live, charge;
    TEST_ASSERT_TRUE(ray_alloc_charge(sizeof values, &charge));
    ray_read_release(&b, child);
    TEST_ASSERT_EQ_U(b.live, unique - charge);
    TEST_ASSERT_NOT_NULL(ray_read_i64(&b, values, 1024));
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);
    TEST_ASSERT_NULL(ray_read_alloc(&b, 0)); /* close ends the admission scope */
    PASS();
}

const test_entry_t read_budget_entries[] = {
    {"reader_proof/budget_edges", budget_edges, setup, teardown},
    {"reader_proof/transitive_ownership", transitive_ownership, setup, teardown},
    {"reader_proof/construction_faults", construction_faults, setup, teardown},
    {"reader_proof/resize_peak", resize_peak, setup, teardown},
    {"reader_proof/oversized_cache", oversized_cache, setup, teardown},
    {"reader_proof/retained_child_admission", retained_child_admission, setup, teardown},
    {"reader_proof/retained_child_reclaim", retained_child_reclaim, setup, teardown},
    {NULL, NULL, NULL, NULL}
};
