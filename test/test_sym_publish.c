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

#define NAMES 300
static void fill_names(char names[NAMES][40], const char* ptrs[NAMES], size_t lengths[NAMES]) {
    for (size_t i = 0; i < NAMES; i++) {
        snprintf(names[i], 40, "proof-value.%zu-with-long-bytes", i);
        ptrs[i] = names[i]; lengths[i] = strlen(names[i]);
    }
}

static test_result_t publication_faults(void) {
    char names[NAMES][40]; const char* ptrs[NAMES]; size_t lengths[NAMES];
    fill_names(names, ptrs, lengths);
    int64_t existing = ray_sym_intern("preserved", 9);
    int64_t dotted_id = ray_sym_intern("existing.path", 13);
    const int64_t* old_segments;
    TEST_ASSERT_EQ_I(ray_sym_segs(dotted_id, &old_segments), 2);
    int64_t saved_segments[] = {old_segments[0], old_segments[1]};
    uint32_t before = ray_sym_count();
    unsigned char snapshot[512];
    int64_t sys_before;
    ray_sys_get_stat(&sys_before, NULL);
    TEST_ASSERT(ray_sym_test_table_size() <= sizeof snapshot, "snapshot fits");
    memcpy(snapshot, (void*)ray_sym_test_table_addr(), ray_sym_test_table_size());
    bool success = false;
    for (size_t fail = 0; fail < 32; fail++) {
        int64_t ids[NAMES];
        for (size_t i = 0; i < NAMES; i++) ids[i] = -99;
        ray_read_budget_t b;
        TEST_ASSERT_TRUE(ray_read_budget_init(&b, 2u << 20));
        b.fail_after = fail;
        success = ray_sym_publish_values(&b, ptrs, lengths, NAMES, UINT32_MAX, ids);
        if (!success) {
            TEST_ASSERT_EQ_I(b.status, RAY_READ_OOM);
            TEST_ASSERT_EQ_U(ray_sym_count(), before);
            TEST_ASSERT_EQ_I(ray_sym_find("preserved", 9), existing);
            TEST_ASSERT(memcmp(snapshot, (void*)ray_sym_test_table_addr(), ray_sym_test_table_size()) == 0,
                        "failed reserve retained no table/arena capacity changes");
            for (size_t i = 0; i < NAMES; i++) {
                TEST_ASSERT_EQ_I(ids[i], -99);
                TEST_ASSERT_EQ_I(ray_sym_find(ptrs[i], lengths[i]), -1);
            }
        } else {
            TEST_ASSERT_EQ_U(ray_sym_count(), before + NAMES);
            for (size_t i = 0; i < NAMES; i++) {
                TEST_ASSERT_EQ_I(ray_sym_find(ptrs[i], lengths[i]), ids[i]);
                TEST_ASSERT_FALSE(ray_sym_is_dotted(ids[i]));
            }
        }
        const int64_t* preserved_segments;
        TEST_ASSERT_EQ_I(ray_sym_segs(dotted_id, &preserved_segments), 2);
        TEST_ASSERT_EQ_I(preserved_segments[0], saved_segments[0]);
        TEST_ASSERT_EQ_I(preserved_segments[1], saved_segments[1]);
        ray_read_budget_close(&b);
        TEST_ASSERT_EQ_U(b.live, 0);
        if (success) break;
        int64_t sys_after;
        ray_sys_get_stat(&sys_after, NULL);
        TEST_ASSERT_EQ_I(sys_after, sys_before);
    }
    TEST_ASSERT_TRUE(success);
    PASS();
}

static test_result_t publication_recheck(void) {
    const char* names[] = {"same", "same", "already", "a.b"};
    size_t lengths[] = {4, 4, 7, 3};
    /* Simulate another request's commit after our input was selected. */
    int64_t old = ray_sym_intern("already", 7);
    uint32_t before = ray_sym_count();
    ray_read_budget_t b;
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
    int64_t ids[] = {-1, -1, -1, -1};
    TEST_ASSERT_FALSE(ray_sym_publish_values(&b, names, lengths, 4, before + 1, ids));
    TEST_ASSERT_EQ_U(ray_sym_count(), before);
    TEST_ASSERT_EQ_I(ids[0], -1);
    ray_read_budget_close(&b);
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
    TEST_ASSERT_TRUE(ray_sym_publish_values(&b, names, lengths, 4, before + 2, ids));
    TEST_ASSERT_EQ_I(ids[0], ids[1]);
    TEST_ASSERT_EQ_I(ids[2], old);
    TEST_ASSERT_EQ_U(ray_sym_count(), before + 2);
    TEST_ASSERT_EQ_I(ray_sym_find("a", 1), -1);
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);
    PASS();
}

static test_result_t publication_fixup(void) {
    const char* names[] = {"ready-for-publication"}; size_t lengths[] = {21};
    ray_read_budget_t b;
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 65536));
    ray_t* atom = ray_read_alloc(&b, 0);
    TEST_ASSERT_NOT_NULL(atom);
    atom->type = -RAY_SYM; atom->i64 = -1; /* private provisional slot */
    ray_t* values[] = {atom};
    ray_t* root = ray_read_list(&b, values, 1);
    TEST_ASSERT_NOT_NULL(root);
    int64_t id;
    TEST_ASSERT_TRUE(ray_sym_publish_values(&b, names, lengths, 1, UINT32_MAX, &id));
    size_t attempts = b.attempts;
    b.fail_after = attempts; /* every later proof allocation would fail */
    atom->i64 = id;
    ray_retain(root);
    TEST_ASSERT_EQ_U(b.attempts, attempts);
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(root))[0]->i64, id);
    ray_release(root);
    PASS();
}

static test_result_t publication_budget_edges(void) {
    size_t page;
    TEST_ASSERT_TRUE(ray_sys_charge(1, &page));
    for (int delta = -1; delta <= 1; delta++) {
        char text[32];
        snprintf(text, sizeof text, "budget-symbol-%d", delta);
        const char* names[] = {text}; size_t lengths[] = {strlen(text)};
        uint32_t before = ray_sym_count();
        ray_read_budget_t b;
        /* Ledger, candidate plan, dedup map, input bytes, string chunk. */
        size_t exact = sizeof(b) + 5 * page;
        TEST_ASSERT_TRUE(ray_read_budget_init(&b, exact + delta));
        int64_t id = -99;
        bool ok = ray_sym_publish_values(&b, names, lengths, 1, UINT32_MAX, &id);
        TEST_ASSERT(b.peak <= b.limit, "publication stayed within admitted peak");
        if (delta < 0) {
            TEST_ASSERT_FALSE(ok);
            TEST_ASSERT_EQ_I(b.status, RAY_READ_LIMIT);
            TEST_ASSERT_EQ_I(id, -99);
            TEST_ASSERT_EQ_U(ray_sym_count(), before);
        } else {
            TEST_ASSERT_TRUE(ok);
            TEST_ASSERT_EQ_I(ray_sym_find(text, lengths[0]), id);
            TEST_ASSERT_EQ_U(b.peak, exact);
        }
        ray_read_budget_close(&b);
        TEST_ASSERT_EQ_U(b.live, 0);
    }
    PASS();
}

#define RACE_NAMES 200
typedef struct {
    _Atomic(bool) start;
    char names[RACE_NAMES][32];
    int64_t ids[RACE_NAMES];
} race_t;
static void intern_racer(void* raw) {
    race_t* race = raw;
    ray_heap_init();
    while (!atomic_load_explicit(&race->start, memory_order_acquire)) {}
    for (size_t i = 0; i < RACE_NAMES; i++)
        race->ids[i] = ray_sym_intern(race->names[i], strlen(race->names[i]));
    ray_heap_destroy();
}
static test_result_t publication_concurrent(void) {
    race_t race;
    atomic_init(&race.start, false);
    const char* names[RACE_NAMES]; size_t lengths[RACE_NAMES];
    int64_t ids[RACE_NAMES];
    for (size_t i = 0; i < RACE_NAMES; i++) {
        snprintf(race.names[i], sizeof race.names[i], "reader-race-%zu", i);
        names[i] = race.names[i]; lengths[i] = strlen(names[i]);
    }
    uint32_t before = ray_sym_count();
    ray_read_budget_t b;
    TEST_ASSERT_TRUE(ray_read_budget_init(&b, 2u << 20));
    ray_thread_t thread;
    TEST_ASSERT_EQ_I(ray_thread_create(&thread, intern_racer, &race), RAY_OK);
    atomic_store_explicit(&race.start, true, memory_order_release);
    bool ok = ray_sym_publish_values(&b, names, lengths, RACE_NAMES,
                                     before + RACE_NAMES, ids);
    TEST_ASSERT_EQ_I(ray_thread_join(thread), RAY_OK);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQ_U(ray_sym_count(), before + RACE_NAMES);
    for (size_t i = 0; i < RACE_NAMES; i++) {
        TEST_ASSERT_EQ_I(race.ids[i], ids[i]);
        TEST_ASSERT_EQ_I(ray_sym_find(names[i], lengths[i]), ids[i]);
    }
    ray_read_budget_close(&b);
    TEST_ASSERT_EQ_U(b.live, 0);
    PASS();
}

const test_entry_t sym_publish_entries[] = {
    {"reader_proof/publication_faults", publication_faults, setup, teardown},
    {"reader_proof/publication_recheck", publication_recheck, setup, teardown},
    {"reader_proof/publication_fixup", publication_fixup, setup, teardown},
    {"reader_proof/publication_budget_edges", publication_budget_edges, setup, teardown},
    {"reader_proof/publication_concurrent", publication_concurrent, setup, teardown},
    {NULL, NULL, NULL, NULL}
};
