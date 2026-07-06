/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
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

/* Embeddable append-only log (store/aof.c).  Deliberately no runtime
 * fixture: the AOF contract promises independence from ray_runtime_t,
 * and these tests double as the proof. */

#define _GNU_SOURCE

#include "test.h"
#include <rayforce.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── Fixture: per-test scratch dir ─────────────────────────────────── */

static char g_aof_dir[512];

static void aof_rm_rf(const char* dir) {
    DIR* d = opendir(dir);
    if (d) {
        struct dirent* ent;
        char path[1024];
        while ((ent = readdir(d)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;
            snprintf(path, sizeof path, "%s/%s", dir, ent->d_name);
            remove(path);
        }
        closedir(d);
    }
    rmdir(dir);
}

static void aof_setup(void) {
    snprintf(g_aof_dir, sizeof g_aof_dir, "/tmp/ray_test_aof_%d", (int)getpid());
    aof_rm_rf(g_aof_dir);
}

static void aof_teardown(void) {
    aof_rm_rf(g_aof_dir);
}

/* ── Scan-collector callback ───────────────────────────────────────── */

typedef struct {
    int64_t lsns[64];
    char    payloads[64][32];
    int64_t lens[64];
    int64_t n;
    int64_t stop_after; /* stop the scan once n reaches this; 0 = never */
} collect_t;

static bool collect_cb(int64_t lsn, const void* payload, int64_t len, void* ctx) {
    collect_t* c = ctx;
    if (c->n < 64) {
        c->lsns[c->n] = lsn;
        c->lens[c->n] = len;
        size_t cp = len < 31 ? (size_t)len : 31;
        memcpy(c->payloads[c->n], payload, cp);
        c->payloads[c->n][cp] = '\0';
    }
    c->n++;
    return c->stop_after == 0 || c->n < c->stop_after;
}

/* Path of the segment whose first LSN is `base`. */
static void seg_path(char* buf, size_t cap, int64_t base) {
    snprintf(buf, cap, "%s/%020lld.aof", g_aof_dir, (long long)base);
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static test_result_t test_aof_roundtrip(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    TEST_ASSERT_EQ_I(ray_aof_append(log, "alpha", 5, &err), 0);
    TEST_ASSERT_EQ_I(ray_aof_append(log, "beta", 4, &err), 1);
    TEST_ASSERT_EQ_I(ray_aof_next_lsn(log), 2);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), 2);
    TEST_ASSERT_EQ_I(c.lsns[0], 0);
    TEST_ASSERT(strcmp(c.payloads[0], "alpha") == 0, "payload 0 mismatch");
    TEST_ASSERT(strcmp(c.payloads[1], "beta") == 0, "payload 1 mismatch");

    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

static test_result_t test_aof_reopen_continues_lsn(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    ray_aof_append(log, "one", 3, &err);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK); /* close syncs */

    log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_EQ_I(ray_aof_next_lsn(log), 1);
    TEST_ASSERT_EQ_I(ray_aof_append(log, "two", 3, &err), 1);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 1, collect_cb, &c, &err), 1);
    TEST_ASSERT_EQ_I(c.lsns[0], 1);
    TEST_ASSERT(strcmp(c.payloads[0], "two") == 0, "payload mismatch");
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

static test_result_t test_aof_torn_tail_truncated(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    ray_aof_append(log, "good-1", 6, &err);
    ray_aof_append(log, "good-2", 6, &err);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);

    /* Crash mid-write: a header fragment lands at the tail. */
    char path[1024];
    seg_path(path, sizeof path, 0);
    FILE* f = fopen(path, "ab");
    TEST_ASSERT_NOT_NULL(f);
    fwrite("\xEE\xFF\x00", 1, 3, f);
    fclose(f);

    log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_EQ_I(ray_aof_next_lsn(log), 2); /* torn tail dropped */
    TEST_ASSERT_EQ_I(ray_aof_append(log, "good-3", 6, &err), 2);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), 3);
    TEST_ASSERT(strcmp(c.payloads[2], "good-3") == 0, "payload mismatch");
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

static test_result_t test_aof_uncommitted_invisible_to_scan(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    ray_aof_append(log, "committed", 9, &err);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    /* Deliberately larger than any stdio buffer, so the bytes reach the
     * filesystem before commit — the audit's counter-example.  The scan
     * must still not see it: visibility is the commit frame, not the
     * accident of buffering. */
    size_t big_len = 512 * 1024;
    char*  big = malloc(big_len);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'B', big_len);
    TEST_ASSERT_EQ_I(ray_aof_append(log, big, (int64_t)big_len, &err), 1);
    free(big);

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), 1);
    TEST_ASSERT(strcmp(c.payloads[0], "committed") == 0, "payload mismatch");

    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);
    collect_t c2 = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c2, &err), 2);
    TEST_ASSERT_EQ_I(c2.lens[1], (int64_t)big_len);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

/* The audit's LSN-reuse scenario: an uncommitted append that reached the
 * filesystem is lost in a crash and its LSN is reissued — which is safe
 * ONLY because no scan could ever have delivered it.  Prove both halves. */
static test_result_t test_aof_crash_reuses_only_unobservable_lsns(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_EQ_I(ray_aof_append(log, "acked", 5, &err), 0);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    size_t big_len = 512 * 1024; /* flushes past stdio buffering */
    char*  big = malloc(big_len);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 'X', big_len);
    TEST_ASSERT_EQ_I(ray_aof_append(log, big, (int64_t)big_len, &err), 1);
    free(big);

    /* Even though the bytes are on disk, LSN 1 is not observable. */
    collect_t before = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &before, &err), 1);

    /* Crash: abandon the writer (leak it — a graceful close would
     * commit).  Recovery truncates the uncommitted suffix and reissues
     * LSN 1 for different data; no reader ever saw the old LSN 1. */
    log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_EQ_I(ray_aof_next_lsn(log), 1);
    TEST_ASSERT_EQ_I(ray_aof_append(log, "replacement", 11, &err), 1);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    collect_t after = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &after, &err), 2);
    TEST_ASSERT(strcmp(after.payloads[1], "replacement") == 0, "payload mismatch");
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

/* Damage BELOW the commit boundary is acknowledged data — open must
 * refuse with RAY_ERR_CORRUPT, never silently truncate. */
static test_result_t test_aof_committed_corruption_fails_open(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    ray_aof_append(log, "aaaa", 4, &err);
    ray_aof_append(log, "bbbb", 4, &err);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK); /* close commits */

    char path[1024];
    seg_path(path, sizeof path, 0);
    FILE* f = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 8 + 1, SEEK_SET); /* into record 0's committed payload */
    int ch = fgetc(f);
    fseek(f, 8 + 1, SEEK_SET);
    fputc(ch ^ 0xFF, f);
    fclose(f);

    TEST_ASSERT_EQ_PTR(ray_aof_open(g_aof_dir, 0, &err), NULL);
    TEST_ASSERT_EQ_I(err, RAY_ERR_CORRUPT);
    PASS();
}

static test_result_t test_aof_rotation(void) {
    ray_err_t err = RAY_OK;
    /* Tiny segments: each "payload-NN" record is 8 + 10 bytes. */
    ray_aof_t* log = ray_aof_open(g_aof_dir, 64, &err);
    TEST_ASSERT_NOT_NULL(log);
    char buf[16];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof buf, "payload-%02d", i);
        TEST_ASSERT_EQ_I(ray_aof_append(log, buf, 10, &err), i);
    }
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    /* More than one segment must exist. */
    char path[1024];
    seg_path(path, sizeof path, 0);
    struct stat st;
    TEST_ASSERT(stat(path, &st) == 0, "first segment missing");
    int64_t segments = 0;
    DIR* d = opendir(g_aof_dir);
    TEST_ASSERT_NOT_NULL(d);
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL)
        if (strstr(ent->d_name, ".aof")) segments++;
    closedir(d);
    TEST_ASSERT(segments > 1, "expected rotation to create multiple segments");

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), 20);
    for (int i = 0; i < 20; i++) TEST_ASSERT_EQ_I(c.lsns[i], i);

    /* Reopen mid-history and continue the sequence. */
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    log = ray_aof_open(g_aof_dir, 64, &err);
    TEST_ASSERT_NOT_NULL(log);
    TEST_ASSERT_EQ_I(ray_aof_next_lsn(log), 20);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

static test_result_t test_aof_scan_from_skips_segments(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 64, &err);
    TEST_ASSERT_NOT_NULL(log);
    char buf[16];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof buf, "payload-%02d", i);
        ray_aof_append(log, buf, 10, &err);
    }
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 17, collect_cb, &c, &err), 3);
    TEST_ASSERT_EQ_I(c.lsns[0], 17);
    TEST_ASSERT(strcmp(c.payloads[0], "payload-17") == 0, "payload mismatch");
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

static test_result_t test_aof_scan_early_stop(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);
    for (int i = 0; i < 10; i++) ray_aof_append(log, "x", 1, &err);
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);

    collect_t c = {0};
    c.stop_after = 4;
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), 4);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    PASS();
}

static test_result_t test_aof_interior_corruption_detected(void) {
    ray_err_t err = RAY_OK;
    ray_aof_t* log = ray_aof_open(g_aof_dir, 64, &err);
    TEST_ASSERT_NOT_NULL(log);
    char buf[16];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof buf, "payload-%02d", i);
        ray_aof_append(log, buf, 10, &err);
    }
    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);
    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);

    /* Flip a payload byte in the FIRST segment (interior of the log). */
    char path[1024];
    seg_path(path, sizeof path, 0);
    FILE* f = fopen(path, "r+b");
    TEST_ASSERT_NOT_NULL(f);
    fseek(f, 8 + 2, SEEK_SET); /* into record 0's payload */
    int ch = fgetc(f);
    fseek(f, 8 + 2, SEEK_SET);
    fputc(ch ^ 0xFF, f);
    fclose(f);

    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), -1);
    TEST_ASSERT_EQ_I(err, RAY_ERR_CORRUPT);
    PASS();
}

static test_result_t test_aof_empty_payload_and_bad_args(void) {
    ray_err_t err = RAY_OK;

    TEST_ASSERT_EQ_PTR(ray_aof_open(NULL, 0, &err), NULL);
    TEST_ASSERT_EQ_I(err, RAY_ERR_DOMAIN);

    ray_aof_t* log = ray_aof_open(g_aof_dir, 0, &err);
    TEST_ASSERT_NOT_NULL(log);

    /* zero-length payload is legal (a marker record) */
    TEST_ASSERT_EQ_I(ray_aof_append(log, NULL, 0, &err), 0);
    TEST_ASSERT_EQ_I(err, RAY_OK);

    /* negative length is not */
    TEST_ASSERT_EQ_I(ray_aof_append(log, "x", -1, &err), -1);
    TEST_ASSERT_EQ_I(err, RAY_ERR_DOMAIN);

    /* NULL payload with positive length is not */
    TEST_ASSERT_EQ_I(ray_aof_append(log, NULL, 3, &err), -1);
    TEST_ASSERT_EQ_I(err, RAY_ERR_DOMAIN);

    TEST_ASSERT_EQ_I(ray_aof_commit(log), RAY_OK);
    collect_t c = {0};
    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, collect_cb, &c, &err), 1);
    TEST_ASSERT_EQ_I(c.lens[0], 0);

    TEST_ASSERT_EQ_I(ray_aof_scan(g_aof_dir, 0, NULL, NULL, &err), -1);
    TEST_ASSERT_EQ_I(err, RAY_ERR_DOMAIN);

    TEST_ASSERT_EQ_I(ray_aof_close(log), RAY_OK);
    TEST_ASSERT_EQ_I(ray_aof_close(NULL), RAY_ERR_DOMAIN);

    /* scan of a directory that never existed delivers zero, no error */
    collect_t c2 = {0};
    TEST_ASSERT_EQ_I(
        ray_aof_scan("/tmp/ray_test_aof_never_existed", 0, collect_cb, &c2, &err), 0);
    TEST_ASSERT_EQ_I(err, RAY_OK);
    PASS();
}

const test_entry_t aof_entries[] = {
    { "aof/roundtrip",                    test_aof_roundtrip,                    aof_setup, aof_teardown },
    { "aof/reopen_continues_lsn",         test_aof_reopen_continues_lsn,         aof_setup, aof_teardown },
    { "aof/torn_tail_truncated",          test_aof_torn_tail_truncated,          aof_setup, aof_teardown },
    { "aof/uncommitted_invisible",        test_aof_uncommitted_invisible_to_scan, aof_setup, aof_teardown },
    { "aof/crash_lsn_reuse_unobservable", test_aof_crash_reuses_only_unobservable_lsns, aof_setup, aof_teardown },
    { "aof/committed_corruption_open",    test_aof_committed_corruption_fails_open, aof_setup, aof_teardown },
    { "aof/rotation",                     test_aof_rotation,                     aof_setup, aof_teardown },
    { "aof/scan_from_skips_segments",     test_aof_scan_from_skips_segments,     aof_setup, aof_teardown },
    { "aof/scan_early_stop",              test_aof_scan_early_stop,              aof_setup, aof_teardown },
    { "aof/interior_corruption_detected", test_aof_interior_corruption_detected, aof_setup, aof_teardown },
    { "aof/empty_payload_and_bad_args",   test_aof_empty_payload_and_bad_args,   aof_setup, aof_teardown },
    { NULL, NULL, NULL, NULL },
};
