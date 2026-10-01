/*
 * test_stress_matrix.c — deterministic scenario matrix for the
 * parted/splayed shared-symfile stress engine (stress_store.c).
 */

#include "test.h"
#include "stress_store.h"
#include <rayforce.h>
#include "store/splay.h"
#include "store/part.h"
#include "mem/heap.h"
#include "table/sym.h"


static void stress_setup(void) {
    ray_heap_init();
    (void)ray_sym_init();
}

static void stress_teardown(void) {
    ray_sym_destroy();
    ray_heap_destroy();
}

/* Wrapper pattern: the _impl body may early-return on TEST_ASSERT failure;
 * the wrapper guarantees stress_destroy (which frees the shadow + oplog and
 * keeps the db dir only when c->failed is set). */

/* Fixture seeds, persists, and both table types load back cleanly. */
static test_result_t test_fixture_roundtrip_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 100, 3, 50), "seed");

    /* live splayed table loads with the shared symfile */
    char livedir[512];
    stress_live_dir(c, livedir, sizeof(livedir));
    ray_t* live = ray_splay_load(livedir, c->sym_path);
    TEST_ASSERT_NOT_NULL(live);
    TEST_ASSERT_FALSE(RAY_IS_ERR(live));
    TEST_ASSERT_EQ_I(ray_table_nrows(live), 100);
    TEST_ASSERT_EQ_I(ray_table_ncols(live), 3);
    ray_release(live);

    /* parted table loads and skips the non-date `live` dir */
    ray_t* parted = ray_read_parted(c->db_root, "hist");
    TEST_ASSERT_NOT_NULL(parted);
    TEST_ASSERT_FALSE(RAY_IS_ERR(parted));
    ray_release(parted);

    PASS();
}

static test_result_t test_fixture_roundtrip(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 42), "init");
    test_result_t r = test_fixture_roundtrip_impl(&c);
    stress_destroy(&c);
    return r;
}

/* Clean fixture verifies clean, both load paths. */
static test_result_t test_verify_clean_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 200, 4, 100), "seed");
    TEST_ASSERT(stress_verify_all(c, false), "verify heap-load");
    TEST_ASSERT(stress_verify_all(c, true), "verify mmap-load");
    PASS();
}

static test_result_t test_verify_clean(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 43), "init");
    test_result_t r = test_verify_clean_impl(&c);
    stress_destroy(&c);
    return r;
}

/* Oracle self-check: the verifier MUST flag a disk/shadow divergence.
 * We mutate the shadow (cheapest way to fake corruption) and expect
 * verify to fail.  The stderr dump below is expected output. */
static test_result_t test_verify_detects_divergence_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 50, 2, 20), "seed");
    TEST_ASSERT(stress_verify_all(c, false), "clean first");

    fprintf(stderr, "\n[stress] NOTE: the next STRESS FAILURE dump is "
                    "an intentional oracle self-check\n");
    int64_t saved = c->live.rows[7].qty;
    c->live.rows[7].qty = saved + 1;
    TEST_ASSERT_FALSE(stress_verify_all(c, false));
    c->live.rows[7].qty = saved;
    c->failed = false; /* undo the keep-dir flag; this was synthetic */

    /* same for a ticker-string divergence */
    fprintf(stderr, "[stress] NOTE: one more intentional self-check dump\n");
    char savedt[STRESS_SYM_MAX];
    memcpy(savedt, c->live.rows[3].ticker, STRESS_SYM_MAX);
    snprintf(c->live.rows[3].ticker, STRESS_SYM_MAX, "bogus_never_interned");
    TEST_ASSERT_FALSE(stress_verify_all(c, false));
    memcpy(c->live.rows[3].ticker, savedt, STRESS_SYM_MAX);
    c->failed = false;

    TEST_ASSERT(stress_verify_all(c, false), "clean again after restore");
    PASS();
}

static test_result_t test_verify_detects_divergence(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 44), "init");
    test_result_t r = test_verify_detects_divergence_impl(&c);
    stress_destroy(&c);
    return r;
}

static const stress_sym_pattern_t k_patterns[] = {
    STRESS_SYMS_NEW, STRESS_SYMS_EXISTING, STRESS_SYMS_MIXED,
    STRESS_SYMS_NULLS,
};
#define NPATTERNS (sizeof(k_patterns) / sizeof(k_patterns[0]))

/* insert x {patterns} x {durable,bulk} x {heap,mmap load}, verify after
 * every combination through both reload paths. */
static test_result_t test_matrix_insert_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 2, 32), "seed");
    for (size_t p = 0; p < NPATTERNS; p++)
        for (int bulk = 0; bulk <= 1; bulk++)
            for (int mm = 0; mm <= 1; mm++) {
                TEST_ASSERT_FMT(stress_op_insert(c, 16, k_patterns[p],
                                                 bulk, mm),
                                "insert pat=%zu bulk=%d mmap=%d", p, bulk, mm);
                TEST_ASSERT_FMT(stress_verify_all(c, false),
                                "verify(heap) after pat=%zu bulk=%d mmap=%d",
                                p, bulk, mm);
                TEST_ASSERT_FMT(stress_verify_all(c, true),
                                "verify(mmap) after pat=%zu bulk=%d mmap=%d",
                                p, bulk, mm);
            }
    PASS();
}

static test_result_t test_matrix_insert(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 100), "init");
    test_result_t r = test_matrix_insert_impl(&c);
    stress_destroy(&c);
    return r;
}

/* upsert x {patterns} x {heap,mmap}, many repetitions so both the update
 * and the append arm are taken. */
static test_result_t test_matrix_upsert_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 2, 32), "seed");
    for (size_t p = 0; p < NPATTERNS; p++)
        for (int mm = 0; mm <= 1; mm++)
            for (int rep = 0; rep < 8; rep++) {
                TEST_ASSERT_FMT(stress_op_upsert(c, k_patterns[p], mm),
                                "upsert pat=%zu mmap=%d rep=%d", p, mm, rep);
                TEST_ASSERT_FMT(stress_verify_all(c, rep & 1),
                                "verify after upsert pat=%zu mmap=%d rep=%d",
                                p, mm, rep);
            }
    PASS();
}

static test_result_t test_matrix_upsert(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 101), "init");
    test_result_t r = test_matrix_upsert_impl(&c);
    stress_destroy(&c);
    return r;
}

/* trim head/tail on live and on every partition, interleaved with verifies.
 * Also trims to empty and rebuilds, covering the zero-row save/load edge. */
static test_result_t test_matrix_trim_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 3, 32), "seed");
    for (int target = -1; target < 3; target++)
        for (int tail = 0; tail <= 1; tail++) {
            TEST_ASSERT_FMT(stress_op_trim(c, target, tail, 5),
                            "trim target=%d tail=%d", target, tail);
            TEST_ASSERT_FMT(stress_verify_all(c, tail),
                            "verify after trim target=%d tail=%d", target,
                            tail);
        }
    /* trim live to empty, verify, then refill */
    TEST_ASSERT(stress_op_trim(c, -1, false, 1000000), "trim to empty");
    TEST_ASSERT(stress_verify_all(c, false), "verify empty live");
    TEST_ASSERT(stress_op_insert(c, 32, STRESS_SYMS_MIXED, false, false),
                "refill");
    TEST_ASSERT(stress_verify_all(c, true), "verify refilled");
    PASS();
}

static test_result_t test_matrix_trim(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 102), "init");
    test_result_t r = test_matrix_trim_impl(&c);
    stress_destroy(&c);
    return r;
}

/* partition append + new-partition growth across patterns; the parted
 * loader must keep accepting the root after every change. */
static test_result_t test_matrix_parted_ops_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 32, 2, 32), "seed");
    for (size_t p = 0; p < NPATTERNS; p++) {
        TEST_ASSERT_FMT(stress_op_part_append(c, (int)(p % 2), 16,
                                              k_patterns[p]),
                        "part_append pat=%zu", p);
        TEST_ASSERT_FMT(stress_verify_all(c, p & 1),
                        "verify after part_append pat=%zu", p);
        TEST_ASSERT_FMT(stress_op_part_new(c, 24, k_patterns[p]),
                        "part_new pat=%zu", p);
        TEST_ASSERT_FMT(stress_verify_all(c, !(p & 1)),
                        "verify after part_new pat=%zu", p);
    }
    PASS();
}

static test_result_t test_matrix_parted_ops(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 103), "init");
    test_result_t r = test_matrix_parted_ops_impl(&c);
    stress_destroy(&c);
    return r;
}

/* every op kind -> restart -> verify (both load paths).  Catches state
 * that only looks right because the writing process's sym table is hot. */
static test_result_t test_matrix_restart_after_each_op_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 2, 32), "seed");

    for (size_t p = 0; p < NPATTERNS; p++) {
        TEST_ASSERT_FMT(stress_op_insert(c, 16, k_patterns[p], p & 1, false),
                        "insert pat=%zu", p);
        TEST_ASSERT(stress_op_restart(c), "restart after insert");
        TEST_ASSERT_FMT(stress_verify_all(c, false),
                        "verify(heap) post-restart insert pat=%zu", p);
        TEST_ASSERT_FMT(stress_verify_all(c, true),
                        "verify(mmap) post-restart insert pat=%zu", p);

        TEST_ASSERT_FMT(stress_op_upsert(c, k_patterns[p], false),
                        "upsert pat=%zu", p);
        TEST_ASSERT(stress_op_restart(c), "restart after upsert");
        TEST_ASSERT_FMT(stress_verify_all(c, p & 1),
                        "verify post-restart upsert pat=%zu", p);

        TEST_ASSERT_FMT(stress_op_part_append(c, 0, 8, k_patterns[p]),
                        "part_append pat=%zu", p);
        TEST_ASSERT(stress_op_restart(c), "restart after part_append");
        TEST_ASSERT_FMT(stress_verify_all(c, !(p & 1)),
                        "verify post-restart part_append pat=%zu", p);

        TEST_ASSERT(stress_op_trim(c, -1, p & 1, 4), "trim");
        TEST_ASSERT(stress_op_restart(c), "restart after trim");
        TEST_ASSERT_FMT(stress_verify_all(c, false),
                        "verify post-restart trim pat=%zu", p);
    }

    TEST_ASSERT(stress_op_part_new(c, 16, STRESS_SYMS_MIXED), "part_new");
    TEST_ASSERT(stress_op_restart(c), "restart after part_new");
    TEST_ASSERT(stress_verify_all(c, true), "verify post-restart part_new");

    PASS();
}

static test_result_t test_matrix_restart_after_each_op(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 104), "init");
    test_result_t r = test_matrix_restart_after_each_op_impl(&c);
    stress_destroy(&c);
    return r;
}

/* Audit-before-flip baseline (sym-domain Phase 2): the CLIENT layout —
 * parted `hist` + splayed `live` sharing ONE symfile — under a dense
 * interleave that alternates live-table ops and partition ops, restarting
 * and verifying through BOTH reload paths along the way.  The oracle
 * stores ticker STRINGS, never sym ids or file positions, so this test's
 * expectations are representation-proof: it must pass unchanged before
 * AND after the per-vocabulary symfile flip (Task 7). */
static test_result_t test_matrix_shared_domain_layout_impl(stress_ctx_t* c) {
    TEST_ASSERT(stress_seed_initial(c, 64, 3, 32), "seed");
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) after seed");
    TEST_ASSERT(stress_verify_all(c, true), "verify(mmap) after seed");

#define VERIFY_BOTH(what)                                                     \
    do {                                                                      \
        TEST_ASSERT_FMT(stress_verify_all(c, false), "verify(heap) %s r=%d",  \
                        what, r);                                             \
        TEST_ASSERT_FMT(stress_verify_all(c, true), "verify(mmap) %s r=%d",   \
                        what, r);                                             \
    } while (0)

    for (int r = 0; r < 2; r++) {
        /* live insert … */
        TEST_ASSERT_FMT(stress_op_insert(c, 16,
                                         r ? STRESS_SYMS_EXISTING
                                           : STRESS_SYMS_NEW,
                                         r, !r),
                        "insert live r=%d", r);
        VERIFY_BOTH("after insert");

        /* … then a partition append into the shared symfile … */
        TEST_ASSERT_FMT(stress_op_part_append(c, r, 12, STRESS_SYMS_MIXED),
                        "part_append r=%d", r);
        VERIFY_BOTH("after part_append");

        /* … live upsert … */
        TEST_ASSERT_FMT(stress_op_upsert(c, r ? STRESS_SYMS_NULLS
                                              : STRESS_SYMS_MIXED, r),
                        "upsert live r=%d", r);
        VERIFY_BOTH("after upsert");

        /* … new partition … */
        TEST_ASSERT_FMT(stress_op_part_new(c, 16, r ? STRESS_SYMS_NEW
                                                    : STRESS_SYMS_EXISTING),
                        "part_new r=%d", r);
        VERIFY_BOTH("after part_new");

        /* … live trim, then partition trim … */
        TEST_ASSERT_FMT(stress_op_trim(c, -1, r, 4), "trim live r=%d", r);
        VERIFY_BOTH("after trim live");
        TEST_ASSERT_FMT(stress_op_trim(c, r, !r, 3), "trim part r=%d", r);
        VERIFY_BOTH("after trim part");

        /* … restart: every live sym id dies, disk is the only truth. */
        TEST_ASSERT_FMT(stress_op_restart(c), "restart r=%d", r);
        VERIFY_BOTH("post-restart");
    }
#undef VERIFY_BOTH

    /* Final cold-start check through both reload paths. */
    TEST_ASSERT(stress_op_restart(c), "final restart");
    TEST_ASSERT(stress_verify_all(c, false), "final verify(heap)");
    TEST_ASSERT(stress_verify_all(c, true), "final verify(mmap)");
    PASS();
}

static test_result_t test_matrix_shared_domain_layout(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 106), "init");
    test_result_t r = test_matrix_shared_domain_layout_impl(&c);
    stress_destroy(&c);
    return r;
}

/* ---- raw on-disk readers (no API, mirroring stress_check_invariants) ---- */

/* SYM width bits from a splayed column file's header attrs byte
 * (bytes 0-15 aux, then mmod/order/type/attrs — store/col.c "Column file
 * format").  Returns RAY_SYM_W8/W16/W32/W64 or -1 on I/O failure. */
static int disk_sym_width(const char* col_path) {
    /* Keep inspecting the raw header, but in the selected generation. */
    char dir[1024], resolved[1024], path[1200];
    const char* slash = strrchr(col_path, '/');
    if (!slash || (size_t)(slash - col_path) >= sizeof(dir)) return -1;
    memcpy(dir, col_path, (size_t)(slash - col_path));
    dir[slash - col_path] = '\0';
    if (ray_splay_resolve_dir(dir, resolved, sizeof(resolved)) != RAY_OK) return -1;
    int n = snprintf(path, sizeof(path), "%s/%s", resolved, slash + 1);
    if (n < 0 || (size_t)n >= sizeof(path)) return -1;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, (long)offsetof(ray_t, attrs), SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    int b = fgetc(f);
    fclose(f);
    if (b == EOF) return -1;
    return b & RAY_SYM_W_MASK;
}

/* Symfile entry count from the raw header ([4B "STRL"][8B count]).
 * Returns -1 on I/O failure or bad magic. */
static int64_t disk_symfile_count(const char* sym_path) {
    FILE* f = fopen(sym_path, "rb");
    if (!f) return -1;
    uint32_t magic = 0;
    int64_t  cnt   = -1;
    size_t ok = fread(&magic, sizeof(magic), 1, f);
    ok += fread(&cnt, sizeof(cnt), 1, f);
    fclose(f);
    if (ok != 2 || magic != 0x4C525453u) return -1;
    return cnt;
}

/* Vocabulary-width stress (sym-domain spec: "width-from-vocabulary"):
 * the on-disk SYM column width tracks the SHARED symfile's vocabulary —
 * not the process dictionary — and mixed-width eras coexist in one
 * parted view: a partition written while the vocabulary fit W8 stays W8
 * on disk and keeps reading correctly after the live table crosses into
 * W16 (its positions are all < 256, still valid in the grown domain).
 * The symfile count equals the tables' vocabulary exactly (every fresh
 * ticker ever generated + the reserved "" at position 0). */
static test_result_t test_matrix_vocab_width_impl(stress_ctx_t* c) {
    char live_col[600], part_col[600];
    snprintf(live_col, sizeof(live_col), "%s/live/ticker", c->db_root);

    /* ── W8 era: small fixture, vocabulary well under 256 ─────────────── */
    TEST_ASSERT(stress_seed_initial(c, 96, 1, 32), "seed");
    /* a partition WRITTEN in the W8 era (before any growth) */
    TEST_ASSERT(stress_op_part_new(c, 24, STRESS_SYMS_MIXED),
                "part_new (W8 era)");
    snprintf(part_col, sizeof(part_col), "%s/%s/hist/ticker", c->db_root,
             c->part_dates[1]);
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) W8 era");
    TEST_ASSERT(stress_verify_all(c, true), "verify(mmap) W8 era");

    int64_t vocab8 = disk_symfile_count(c->sym_path);
    TEST_ASSERT_FMT(vocab8 > 0 && vocab8 <= 255,
                    "W8-era vocabulary out of range: %lld", (long long)vocab8);
    /* symfile count == the tables' vocabulary, NOT the global dictionary
     * (which also holds column names, builtins, session noise) */
    TEST_ASSERT_EQ_I(vocab8, (int64_t)c->sym_uniq + 1);
    TEST_ASSERT_EQ_I(disk_sym_width(live_col), RAY_SYM_W8);
    TEST_ASSERT_EQ_I(disk_sym_width(part_col), RAY_SYM_W8);

    /* ── cross 255: 256 fresh tickers guarantee the W16 boundary ──────── */
    TEST_ASSERT(stress_op_sym_grow(c, 256), "sym_grow -> W16");
    int64_t vocab16 = disk_symfile_count(c->sym_path);
    TEST_ASSERT_EQ_I(vocab16, vocab8 + 256);
    TEST_ASSERT_FMT(vocab16 > 255, "no W16 crossing: %lld",
                    (long long)vocab16);
    TEST_ASSERT_EQ_I(vocab16, (int64_t)c->sym_uniq + 1);
    /* live re-encoded at W16; the W8-era partition file is UNTOUCHED */
    TEST_ASSERT_EQ_I(disk_sym_width(live_col), RAY_SYM_W16);
    TEST_ASSERT_EQ_I(disk_sym_width(part_col), RAY_SYM_W8);
    /* ...and the W8-era partition reads correctly alongside W16 data */
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) mixed W8/W16");
    TEST_ASSERT(stress_verify_all(c, true), "verify(mmap) mixed W8/W16");
    TEST_ASSERT(stress_op_restart(c), "restart in W16 era");
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) post-restart W16");
    TEST_ASSERT(stress_verify_all(c, true), "verify(mmap) post-restart W16");

    /* ── appending to the W8-era partition rewrites it at W16 ─────────── */
    TEST_ASSERT(stress_op_part_append(c, 1, 8, STRESS_SYMS_EXISTING),
                "part_append in W16 era");
    TEST_ASSERT_EQ_I(disk_sym_width(part_col), RAY_SYM_W16);
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) after rewrite");
    TEST_ASSERT(stress_verify_all(c, true), "verify(mmap) after rewrite");

    /* ── cross 65535 the same way (W32) ───────────────────────────────── */
    TEST_ASSERT(stress_op_sym_grow(c, 65536 - vocab16 + 8), "sym_grow -> W32");
    int64_t vocab32 = disk_symfile_count(c->sym_path);
    TEST_ASSERT_FMT(vocab32 > 65535, "no W32 crossing: %lld",
                    (long long)vocab32);
    TEST_ASSERT_EQ_I(vocab32, (int64_t)c->sym_uniq + 1);
    TEST_ASSERT_EQ_I(disk_sym_width(live_col), RAY_SYM_W32);
    /* the partition last written in the W16 era stays W16 on disk */
    TEST_ASSERT_EQ_I(disk_sym_width(part_col), RAY_SYM_W16);
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) mixed W16/W32");
    TEST_ASSERT(stress_verify_all(c, true), "verify(mmap) mixed W16/W32");
    TEST_ASSERT(stress_op_restart(c), "restart in W32 era");
    TEST_ASSERT(stress_verify_all(c, false), "verify(heap) post-restart W32");

    PASS();
}

static test_result_t test_matrix_vocab_width(void) {
    stress_ctx_t c;
    TEST_ASSERT(stress_init(&c, stress_db_path("matrix"), 107), "init");
    test_result_t r = test_matrix_vocab_width_impl(&c);
    stress_destroy(&c);
    return r;
}

const test_entry_t stress_matrix_entries[] = {
    { "stress/fixture_roundtrip", test_fixture_roundtrip, stress_setup, stress_teardown },
    { "stress/verify_clean",             test_verify_clean,             stress_setup, stress_teardown },
    { "stress/verify_detects_divergence", test_verify_detects_divergence, stress_setup, stress_teardown },
    { "stress/matrix_insert", test_matrix_insert, stress_setup, stress_teardown },
    { "stress/matrix_upsert", test_matrix_upsert, stress_setup, stress_teardown },
    { "stress/matrix_trim",       test_matrix_trim,       stress_setup, stress_teardown },
    { "stress/matrix_parted_ops", test_matrix_parted_ops, stress_setup, stress_teardown },
    { "stress/matrix_restart_after_each_op", test_matrix_restart_after_each_op, stress_setup, stress_teardown },
    { "stress/matrix_shared_domain_layout", test_matrix_shared_domain_layout, stress_setup, stress_teardown },
    { "stress/matrix_vocab_width", test_matrix_vocab_width, stress_setup, stress_teardown },
    { NULL, NULL, NULL, NULL },
};
