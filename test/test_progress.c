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

/*
 * Tests for src/core/progress.c — pull-based, main-thread-only progress
 * reporting. The module's I/O is entirely through a user callback, so the
 * tests record snapshots into a fixed-size buffer (no dynamic allocation —
 * conforms to the "no libc malloc in tests" rule) and assert on the buffer
 * after driving the public API.
 *
 * Timing: min_ms / tick_ms thresholds are exercised with millisecond-scale
 * sleeps. The module reads CLOCK_MONOTONIC_COARSE which on Linux runs at
 * jiffy resolution (~1–4 ms typical), so all timing tests use generous
 * margins (min_ms ≥ 30 ms, sleeps ≥ ~2× the threshold) to stay reliable
 * under load.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "core/profile.h"

#include <string.h>
#include <time.h>

/* ---- Snapshot recorder -------------------------------------------------- */

#define RECORDER_CAP 32

typedef struct {
    char        op_name[64];
    char        phase[64];
    uint64_t    rows_done;
    uint64_t    rows_total;
    double      elapsed_sec;
    int64_t     mem_used;
    int64_t     mem_budget;
    bool        final;
} recorded_t;

typedef struct {
    int        n;
    recorded_t snaps[RECORDER_CAP];
    int        magic;
} recorder_t;

static const int RECORDER_MAGIC = 0x5253474e; /* "RSGN" */

static void recorder_reset(recorder_t* r) {
    r->n = 0;
    r->magic = RECORDER_MAGIC;
    memset(r->snaps, 0, sizeof r->snaps);
}

static void record_cb(const ray_progress_t* snap, void* user) {
    recorder_t* r = (recorder_t*)user;
    if (r->n >= RECORDER_CAP) return;
    recorded_t* s = &r->snaps[r->n++];
    /* Defensive copy: snap->op_name / phase point at caller-owned storage,
     * so we copy the bytes here. snprintf-with-precision-zero on NULL is UB,
     * so guard explicitly. */
    if (snap->op_name) {
        size_t n = strlen(snap->op_name);
        if (n >= sizeof s->op_name) n = sizeof s->op_name - 1;
        memcpy(s->op_name, snap->op_name, n);
        s->op_name[n] = '\0';
    }
    if (snap->phase) {
        size_t n = strlen(snap->phase);
        if (n >= sizeof s->phase) n = sizeof s->phase - 1;
        memcpy(s->phase, snap->phase, n);
        s->phase[n] = '\0';
    }
    s->rows_done   = snap->rows_done;
    s->rows_total  = snap->rows_total;
    s->elapsed_sec = snap->elapsed_sec;
    s->mem_used    = snap->mem_used;
    s->mem_budget  = snap->mem_budget;
    s->final       = snap->final;
}

/* Sentinel callback that records nothing — used to verify magic-protected
 * cells are not stomped when callback is intentionally cleared. */
static void noop_cb(const ray_progress_t* snap, void* user) {
    (void)snap;
    (void)user;
}

static void sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/* ---- Setup / Teardown --------------------------------------------------- */

/* progress.c calls ray_mem_stats() / ray_mem_budget() inside fire(); both
 * require the heap to be initialised. Every test does begin/end on the heap
 * so callbacks can fire safely. */
static void progress_setup(void) {
    ray_heap_init();
    /* Always reset module state from any prior test by clearing the
     * callback then issuing an end with no callback (which clears the
     * start_ns flag). */
    ray_progress_set_callback(NULL, NULL, 0, 0);
    ray_progress_end();
}

static void progress_teardown(void) {
    /* Disconnect callback before the recorder (a stack object) goes out of
     * scope. Any subsequent stray fire() would otherwise deref dead memory. */
    ray_progress_set_callback(NULL, NULL, 0, 0);
    ray_progress_end();
    ray_heap_destroy();
}

/* ---- Tests -------------------------------------------------------------- */

/* NULL callback path: every public entry is a no-op — no crash, no fire,
 * no state change observable from outside. Drives the early-return
 * branches in update / label / end. */
static test_result_t test_progress_null_callback(void) {
    /* Default state — no callback registered (setup cleared it). */
    ray_progress_update("scan", "init", 5, 100);
    ray_progress_update(NULL, NULL, 10, 100);
    ray_progress_label("group", "dedupe");
    ray_progress_end();
    /* If we got here without crashing, the early-return paths held. */
    PASS();
}

/* Callback fires on ray_progress_end only after enough time elapsed.
 * Drives the 'g_showing' branch in ray_progress_end and the final=true
 * snapshot field. */
static test_result_t test_progress_end_fires_final(void) {
    recorder_t r;
    recorder_reset(&r);
    /* Tiny min_ms / tick so we don't spend seconds in tests. */
    ray_progress_set_callback(record_cb, &r, 30, 10);

    ray_progress_update("scan", "rows", 0, 1000);
    /* No fire yet — still inside min_ms window. */
    TEST_ASSERT_EQ_I(r.n, 0);

    sleep_ms(60);
    ray_progress_update("scan", "rows", 500, 1000);
    /* First fire passes both gates. */
    TEST_ASSERT_EQ_I(r.n, 1);
    TEST_ASSERT_FALSE(r.snaps[0].final);
    TEST_ASSERT_STR_EQ(r.snaps[0].op_name, "scan");
    TEST_ASSERT_STR_EQ(r.snaps[0].phase, "rows");
    TEST_ASSERT_EQ_U(r.snaps[0].rows_done, 500);
    TEST_ASSERT_EQ_U(r.snaps[0].rows_total, 1000);

    ray_progress_end();
    /* Final fire — should set final=true and bring rows_done up to total. */
    TEST_ASSERT_EQ_I(r.n, 2);
    TEST_ASSERT_TRUE(r.snaps[1].final);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_done, 1000);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_total, 1000);

    PASS();
}

/* Short query (under min_ms) fires zero callbacks, including no final tick.
 * Drives the 'g_showing == false' path in ray_progress_end. */
static test_result_t test_progress_short_query_no_fire(void) {
    recorder_t r;
    recorder_reset(&r);
    /* Set min_ms high enough that we cannot exceed it during this test. */
    ray_progress_set_callback(record_cb, &r, 5000, 50);

    ray_progress_update("scan", "rows", 0, 100);
    sleep_ms(20);
    ray_progress_update("scan", "rows", 100, 100);
    ray_progress_end();

    TEST_ASSERT_EQ_I(r.n, 0);
    PASS();
}

/* tick_ms cadence: rapid updates shouldn't all fire — only those at least
 * tick_ms apart. Drives the 'since_last < g_tick_ms' guard in
 * ray_progress_update. */
static test_result_t test_progress_tick_throttle(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 50);

    ray_progress_update("scan", "rows", 0, 1000);
    /* Cross min_ms — first update past it fires once. */
    sleep_ms(60);
    ray_progress_update("scan", "rows", 100, 1000);
    TEST_ASSERT_EQ_I(r.n, 1);

    /* Three rapid updates well under tick_ms — none should fire. */
    ray_progress_update("scan", "rows", 200, 1000);
    ray_progress_update("scan", "rows", 300, 1000);
    ray_progress_update("scan", "rows", 400, 1000);
    TEST_ASSERT_EQ_I(r.n, 1);

    /* After tick_ms passes, next update fires. */
    sleep_ms(70);
    ray_progress_update("scan", "rows", 800, 1000);
    TEST_ASSERT_EQ_I(r.n, 2);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_done, 800);

    /* Final tick. */
    ray_progress_end();
    TEST_ASSERT_EQ_I(r.n, 3);
    TEST_ASSERT_TRUE(r.snaps[2].final);

    PASS();
}

/* op_name / phase NULL-keep semantics in ray_progress_update: a NULL keeps
 * the previously stored value, while ray_progress_label always overwrites
 * phase. Together these cover the "if op_name" / "if phase" guards in
 * update vs the unconditional store in label. */
static test_result_t test_progress_null_keep_semantics(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    ray_progress_update("scan", "rows", 0, 100);
    sleep_ms(60);
    ray_progress_update("scan", "rows", 50, 100);
    TEST_ASSERT_EQ_I(r.n, 1);
    TEST_ASSERT_STR_EQ(r.snaps[0].op_name, "scan");
    TEST_ASSERT_STR_EQ(r.snaps[0].phase, "rows");

    /* NULL op_name and NULL phase — should keep "scan"/"rows". */
    sleep_ms(20);
    ray_progress_update(NULL, NULL, 70, 100);
    TEST_ASSERT_EQ_I(r.n, 2);
    TEST_ASSERT_STR_EQ(r.snaps[1].op_name, "scan");
    TEST_ASSERT_STR_EQ(r.snaps[1].phase, "rows");
    TEST_ASSERT_EQ_U(r.snaps[1].rows_done, 70);

    ray_progress_end();
    PASS();
}

/* ray_progress_label drops the previous phase when called with NULL phase
 * (unlike update which keeps it) AND resets counters. Also fires if the
 * gates pass. */
static test_result_t test_progress_label_resets(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    ray_progress_update("scan", "rows", 50, 100);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 60, 100);
    TEST_ASSERT_EQ_I(r.n, 1);

    /* label → new op, NULL phase clears stale phase, counters reset. */
    sleep_ms(20);
    ray_progress_label("group", NULL);
    TEST_ASSERT_EQ_I(r.n, 2);
    TEST_ASSERT_STR_EQ(r.snaps[1].op_name, "group");
    TEST_ASSERT_STR_EQ(r.snaps[1].phase, "");          /* fmt'd from NULL */
    TEST_ASSERT_EQ_U(r.snaps[1].rows_done, 0);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_total, 0);

    ray_progress_end();
    PASS();
}

/* min_ms = 0 / tick_ms = 0 in set_callback should preserve module defaults
 * (2000 / 100 ms). Confirm by setting NON-default values, then "resetting"
 * with zeros and observing that the previously set non-defaults are kept. */
static test_result_t test_progress_set_callback_zero_keeps_existing(void) {
    recorder_t r;
    recorder_reset(&r);
    /* Set explicit small thresholds. */
    ray_progress_set_callback(record_cb, &r, 30, 10);
    /* Now "update" the callback registration with zeros — should keep
     * the previously-set 30 / 10 values, not snap back to defaults. */
    ray_progress_set_callback(record_cb, &r, 0, 0);

    ray_progress_update("scan", NULL, 0, 100);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 50, 100);
    /* If thresholds had snapped back to 2000ms, no fire would happen here. */
    TEST_ASSERT_EQ_I(r.n, 1);

    ray_progress_end();
    PASS();
}

/* Snapshot field plumbing: elapsed_sec strictly increases across fires;
 * mem_used and mem_budget are populated. */
static test_result_t test_progress_snapshot_fields(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    ray_progress_update("op", "ph", 1, 10);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 5, 10);
    sleep_ms(20);
    ray_progress_update(NULL, NULL, 8, 10);
    ray_progress_end();

    /* At least 2 fires (one mid, one final). */
    TEST_ASSERT_TRUE(r.n >= 2);

    /* elapsed_sec is non-decreasing and non-negative. */
    for (int i = 0; i < r.n; i++) {
        TEST_ASSERT_TRUE(r.snaps[i].elapsed_sec >= 0.0);
    }
    for (int i = 1; i < r.n; i++) {
        TEST_ASSERT_TRUE(r.snaps[i].elapsed_sec >= r.snaps[i-1].elapsed_sec);
    }

    /* mem_budget should match the live API result. */
    int64_t budget_now = ray_mem_budget();
    for (int i = 0; i < r.n; i++) {
        TEST_ASSERT_EQ_I(r.snaps[i].mem_budget, budget_now);
    }

    /* mem_used non-negative — heap is initialized so stats are available. */
    for (int i = 0; i < r.n; i++) {
        TEST_ASSERT_TRUE(r.snaps[i].mem_used >= 0);
    }

    /* Last fire is final. */
    TEST_ASSERT_TRUE(r.snaps[r.n - 1].final);

    PASS();
}

/* Back-to-back begin/end pairs: state is fully reset, including g_showing,
 * so a second short query under min_ms again fires nothing. */
static test_result_t test_progress_state_reset_between_queries(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    /* First query — long enough to fire. */
    ray_progress_update("op1", "p1", 0, 10);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 5, 10);
    ray_progress_end();
    int after_first = r.n;
    TEST_ASSERT_TRUE(after_first >= 2);

    /* Second query — short enough to NOT fire. If state hadn't reset,
     * g_showing would still be true and end() would emit a final tick. */
    ray_progress_set_callback(record_cb, &r, 5000, 50);
    ray_progress_update("op2", "p2", 0, 10);
    sleep_ms(20);
    ray_progress_update(NULL, NULL, 5, 10);
    ray_progress_end();
    TEST_ASSERT_EQ_I(r.n, after_first);

    PASS();
}

/* End without callback hits the early-return branch that only clears
 * g_start_ns. After a no-callback end, registering a callback and running
 * a normal query still works (clock lazy-restarts on the next update). */
static test_result_t test_progress_end_without_callback(void) {
    recorder_t r;
    recorder_reset(&r);

    /* No callback yet — set one then drive a partial query. */
    ray_progress_set_callback(record_cb, &r, 30, 10);
    ray_progress_update("scan", "rows", 0, 10);
    /* Detach callback BEFORE end. ray_progress_end with no callback hits
     * the !g_cb early-return that only zeros g_start_ns. */
    ray_progress_set_callback(NULL, NULL, 30, 10);
    ray_progress_end();

    /* Now register a fresh callback and drive a complete query. The lazy
     * clock-start branch in update should kick in, since g_start_ns is 0. */
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);
    ray_progress_update("op2", "p2", 0, 100);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 50, 100);
    ray_progress_end();

    TEST_ASSERT_TRUE(r.n >= 2);
    TEST_ASSERT_STR_EQ(r.snaps[0].op_name, "op2");
    TEST_ASSERT_TRUE(r.snaps[r.n - 1].final);
    PASS();
}

/* End fires a "100% normalized" tick when rows_total > 0 — rows_done is
 * forced up to rows_total even if it lagged. */
static test_result_t test_progress_end_normalizes_rows_done(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    ray_progress_update("scan", "rows", 0, 100);
    sleep_ms(60);
    /* Final mid-tick advances to a partial value, NOT 100. */
    ray_progress_update(NULL, NULL, 70, 100);
    TEST_ASSERT_EQ_I(r.n, 1);
    TEST_ASSERT_EQ_U(r.snaps[0].rows_done, 70);

    ray_progress_end();
    /* end() should have promoted rows_done to rows_total. */
    TEST_ASSERT_EQ_I(r.n, 2);
    TEST_ASSERT_TRUE(r.snaps[1].final);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_done, 100);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_total, 100);

    PASS();
}

/* End with rows_total = 0 (indeterminate progress): rows_done is left
 * untouched in the final tick. Drives the 'if (g_rows_total)' guard in
 * ray_progress_end. */
static test_result_t test_progress_end_indeterminate(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    ray_progress_update("scan", "rows", 0, 0);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 42, 0);
    TEST_ASSERT_EQ_I(r.n, 1);

    ray_progress_end();
    TEST_ASSERT_EQ_I(r.n, 2);
    TEST_ASSERT_TRUE(r.snaps[1].final);
    /* rows_total stays 0; rows_done remains 42 (NOT promoted). */
    TEST_ASSERT_EQ_U(r.snaps[1].rows_total, 0);
    TEST_ASSERT_EQ_U(r.snaps[1].rows_done, 42);

    PASS();
}

/* Lazy clock-start happens only on the first call after end (or first call
 * ever). The label() entry should also lazy-start when g_start_ns == 0. */
static test_result_t test_progress_label_lazy_start(void) {
    recorder_t r;
    recorder_reset(&r);
    ray_progress_set_callback(record_cb, &r, 30, 10);

    /* First call into the API after a clean end is a label() — should still
     * lazy-start the clock without crashing or producing a bogus elapsed. */
    ray_progress_label("scan", "init");
    /* No fire yet — inside min_ms window. */
    TEST_ASSERT_EQ_I(r.n, 0);

    sleep_ms(60);
    ray_progress_update(NULL, NULL, 5, 10);
    TEST_ASSERT_EQ_I(r.n, 1);
    /* The op_name from label() must have been preserved across the
     * subsequent NULL-keep update. */
    TEST_ASSERT_STR_EQ(r.snaps[0].op_name, "scan");
    TEST_ASSERT_STR_EQ(r.snaps[0].phase, "init");
    /* elapsed_sec is plausible: well above min_ms (~30 ms = 0.030 s) but
     * well below absurd values (10s). This guards against a bug where the
     * label-path lazy-start failed to set g_start_ns. */
    TEST_ASSERT_TRUE(r.snaps[0].elapsed_sec >= 0.03);
    TEST_ASSERT_TRUE(r.snaps[0].elapsed_sec < 10.0);

    ray_progress_end();
    PASS();
}

/* user pointer is plumbed through to the callback unchanged. */
static test_result_t test_progress_user_pointer_plumbing(void) {
    recorder_t r;
    recorder_reset(&r);

    ray_progress_set_callback(record_cb, &r, 30, 10);
    ray_progress_update("op", "ph", 0, 100);
    sleep_ms(60);
    ray_progress_update(NULL, NULL, 1, 100);
    TEST_ASSERT_EQ_I(r.n, 1);
    /* magic was set in recorder_reset and must still be present, proving
     * the callback received the real user pointer (not garbage). */
    TEST_ASSERT_EQ_I(r.magic, RECORDER_MAGIC);

    ray_progress_end();
    PASS();
}

/* Setting a noop callback then end (mid-query, not under min_ms) takes the
 * "showing == false" branch in end. */
static test_result_t test_progress_end_branch_no_show(void) {
    /* Use noop_cb so we don't pollute another recorder. */
    ray_progress_set_callback(noop_cb, NULL, 5000, 50);

    /* Drive an update that will NOT fire (under min_ms). */
    ray_progress_update("op", "ph", 0, 10);
    sleep_ms(5);
    ray_progress_update(NULL, NULL, 1, 10);

    /* End hits the !g_showing path — no fire, just state clear. Just ensure
     * no crash. */
    ray_progress_end();
    PASS();
}

/* ---- core/profile.h inline progress helpers ---------------------------- */

/* These cover the three uncovered static-inline functions in
 * src/core/profile.h: ray_profile_progress_{begin,advance,end}.  The
 * functions guard on g_ray_profile.active and on g_ray_profile.progress_cb.
 * Tests below drive both the inactive-no-op path and the active path with
 * a stub callback, plus the throttle gate inside _advance. */

static int64_t      g_test_prog_done;
static int64_t      g_test_prog_total;
static const char*  g_test_prog_label;
static int          g_test_prog_calls;

static void test_profile_progress_cb(int64_t done, int64_t total, const char* label) {
    g_test_prog_done   = done;
    g_test_prog_total  = total;
    g_test_prog_label  = label;
    g_test_prog_calls += 1;
}

static void profile_inline_teardown(void) {
    g_ray_profile.active        = false;
    g_ray_profile.progress_cb   = NULL;
    g_ray_profile.progress_label = NULL;
    g_ray_profile.progress_total = 0;
    g_ray_profile.progress_done  = 0;
    g_ray_profile.progress_last_render = 0;
}

static test_result_t test_profile_inline_progress_begin(void) {
    /* Inactive path: no fields touched. */
    g_ray_profile.active = false;
    g_ray_profile.progress_label = (const char*)0xdeadbeefULL;
    g_ray_profile.progress_total = 42;
    g_ray_profile.progress_done  = 7;
    ray_profile_progress_begin("ignored", 999);
    TEST_ASSERT_EQ_PTR(g_ray_profile.progress_label, (const char*)0xdeadbeefULL);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_total, 42);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done,   7);

    /* Active path: fields populated. */
    ray_profile_reset();
    g_ray_profile.active = true;
    ray_profile_progress_begin("loading", 100);
    TEST_ASSERT_EQ_PTR(g_ray_profile.progress_label, "loading");
    TEST_ASSERT_EQ_I(g_ray_profile.progress_total, 100);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done,   0);
    PASS();
}

static test_result_t test_profile_inline_progress_advance(void) {
    /* Inactive path: progress_done unchanged. */
    ray_profile_reset();
    g_ray_profile.active        = false;
    g_ray_profile.progress_done = 5;
    ray_profile_progress_advance(10);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done, 5);

    /* Active + NULL callback: counter advances, no fire. */
    ray_profile_reset();
    g_ray_profile.active      = true;
    g_ray_profile.progress_cb = NULL;
    ray_profile_progress_begin("phase", 100);
    ray_profile_progress_advance(3);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done, 3);

    /* Active + callback + total>0: first advance fires (last_render==0). */
    g_test_prog_done = g_test_prog_total = 0;
    g_test_prog_label = NULL;
    g_test_prog_calls = 0;
    ray_profile_reset();
    g_ray_profile.active      = true;
    g_ray_profile.progress_cb = test_profile_progress_cb;
    ray_profile_progress_begin("work", 50);
    ray_profile_progress_advance(7);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done, 7);
    TEST_ASSERT_EQ_I(g_test_prog_calls, 1);
    TEST_ASSERT_EQ_I(g_test_prog_done,  7);
    TEST_ASSERT_EQ_I(g_test_prog_total, 50);
    TEST_ASSERT_STR_EQ(g_test_prog_label, "work");

    /* Throttled: second advance within 100ms must not fire. */
    ray_profile_progress_advance(2);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done, 9);
    TEST_ASSERT_EQ_I(g_test_prog_calls, 1);

    /* total<=0 path: callback set, but no total → no fire. */
    ray_profile_reset();
    g_ray_profile.active        = true;
    g_ray_profile.progress_cb   = test_profile_progress_cb;
    g_ray_profile.progress_total = 0;
    g_test_prog_calls = 0;
    ray_profile_progress_advance(1);
    TEST_ASSERT_EQ_I(g_test_prog_calls, 0);
    PASS();
}

static test_result_t test_profile_inline_progress_end(void) {
    /* Inactive: fields preserved. */
    g_ray_profile.active        = false;
    g_ray_profile.progress_label = "still-here";
    g_ray_profile.progress_total = 11;
    g_ray_profile.progress_done  = 4;
    ray_profile_progress_end();
    TEST_ASSERT_STR_EQ(g_ray_profile.progress_label, "still-here");
    TEST_ASSERT_EQ_I(g_ray_profile.progress_total, 11);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done,   4);

    /* Active: fields cleared. */
    ray_profile_reset();
    g_ray_profile.active = true;
    ray_profile_progress_begin("done-soon", 200);
    ray_profile_progress_end();
    TEST_ASSERT_NULL(g_ray_profile.progress_label);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_total, 0);
    TEST_ASSERT_EQ_I(g_ray_profile.progress_done,  0);
    PASS();
}

/* ---- Suite definition --------------------------------------------------- */

const test_entry_t progress_entries[] = {
    { "progress/null_callback",            test_progress_null_callback,
      progress_setup, progress_teardown },
    { "progress/end_fires_final",          test_progress_end_fires_final,
      progress_setup, progress_teardown },
    { "progress/short_query_no_fire",      test_progress_short_query_no_fire,
      progress_setup, progress_teardown },
    { "progress/tick_throttle",            test_progress_tick_throttle,
      progress_setup, progress_teardown },
    { "progress/null_keep_semantics",      test_progress_null_keep_semantics,
      progress_setup, progress_teardown },
    { "progress/label_resets",             test_progress_label_resets,
      progress_setup, progress_teardown },
    { "progress/set_callback_zero_keeps",  test_progress_set_callback_zero_keeps_existing,
      progress_setup, progress_teardown },
    { "progress/snapshot_fields",          test_progress_snapshot_fields,
      progress_setup, progress_teardown },
    { "progress/state_reset_between",      test_progress_state_reset_between_queries,
      progress_setup, progress_teardown },
    { "progress/end_without_callback",     test_progress_end_without_callback,
      progress_setup, progress_teardown },
    { "progress/end_normalizes_rows_done", test_progress_end_normalizes_rows_done,
      progress_setup, progress_teardown },
    { "progress/end_indeterminate",        test_progress_end_indeterminate,
      progress_setup, progress_teardown },
    { "progress/label_lazy_start",         test_progress_label_lazy_start,
      progress_setup, progress_teardown },
    { "progress/user_pointer_plumbing",    test_progress_user_pointer_plumbing,
      progress_setup, progress_teardown },
    { "progress/end_branch_no_show",       test_progress_end_branch_no_show,
      progress_setup, progress_teardown },

    /* core/profile.h inline progress helpers */
    { "profile/inline/progress_begin",     test_profile_inline_progress_begin,
      NULL, profile_inline_teardown },
    { "profile/inline/progress_advance",   test_profile_inline_progress_advance,
      NULL, profile_inline_teardown },
    { "profile/inline/progress_end",       test_profile_inline_progress_end,
      NULL, profile_inline_teardown },

    { NULL, NULL, NULL, NULL },
};
