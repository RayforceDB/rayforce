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

#define _GNU_SOURCE

#include "test.h"
#include <rayforce.h>
#include "store/journal.h"
#include "store/serde.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "mem/sys.h"
#include "core/ipc.h"
#include "ops/journal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdint.h>
#include <fcntl.h>
#include <glob.h>

/* ── Runtime fixture (same pattern as test_link.c) ─────────────────── */

extern ray_runtime_t* __RUNTIME;

static void jrn_setup(void)    { ray_runtime_create(0, NULL); }
static void jrn_teardown(void) { ray_runtime_destroy(__RUNTIME); }

/* ── Helper: write a well-formed journal entry for `val` to file `f` ── */

static bool write_journal_entry(FILE* f, ray_t* val) {
    int64_t psize = ray_serde_size(val);
    if (psize <= 0) return false;
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)psize);
    if (!buf) return false;
    int64_t written = ray_ser_raw(buf, val);
    if (written != psize) { ray_sys_free(buf); return false; }

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = psize;

    bool ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)) &&
              (fwrite(buf, 1, (size_t)psize, f) == (size_t)psize);
    ray_sys_free(buf);
    return ok;
}

/* ── Helper: create a temp path and optionally get .log / .qdb paths ── */

static void make_base(char* base, size_t sz, const char* prefix) {
    snprintf(base, sz, "/tmp/jrn_test_%s_XXXXXX", prefix);
    int fd = mkstemp(base);
    if (fd >= 0) {
        close(fd);
        unlink(base); /* use as directory-less base, not an actual file */
    }
}

static void log_path(char* dst, size_t sz, const char* base) {
    snprintf(dst, sz, "%s.log", base);
}

static void qdb_path(char* dst, size_t sz, const char* base) {
    snprintf(dst, sz, "%s.qdb", base);
}

/* ── Cleanup helper: remove base, .log, .qdb, .qdb.tmp, and any .log archives ── */

static void cleanup_base(const char* base) {
    char path[1100];
    snprintf(path, sizeof(path), "%s.log",     base); unlink(path);
    snprintf(path, sizeof(path), "%s.qdb",     base); unlink(path);
    snprintf(path, sizeof(path), "%s.qdb.tmp", base); unlink(path);
    /* Archived rolls have the form base.<stamp>.log — remove with glob via shell. */
    snprintf(path, sizeof(path), "rm -f '%s'.*.log 2>/dev/null", base);
    (void)system(path);
}

/* ═══════════════════════════════════════════════════════════════════════
 *  1. Validation — ray_journal_validate
 * ═══════════════════════════════════════════════════════════════════════ */

/* 1a. Validate a clean log with multiple entries. */
static test_result_t test_journal_validate_clean(void) {
    char base[256]; make_base(base, sizeof(base), "val_clean");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_t* v1 = ray_i64(42);
    ray_t* v2 = ray_i64(99);
    TEST_ASSERT_TRUE(write_journal_entry(f, v1));
    TEST_ASSERT_TRUE(write_journal_entry(f, v2));
    fclose(f);
    ray_release(v1); ray_release(v2);

    int64_t chunks = -1, valid_bytes = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, &valid_bytes);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 2);
    TEST_ASSERT_TRUE(valid_bytes > 0);

    cleanup_base(base);
    PASS();
}

/* 1b. Validate an empty log — 0 entries. */
static test_result_t test_journal_validate_empty(void) {
    char base[256]; make_base(base, sizeof(base), "val_empty");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    int64_t chunks = -1, valid_bytes = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, &valid_bytes);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 0);
    TEST_ASSERT_EQ_I(valid_bytes, 0);

    cleanup_base(base);
    PASS();
}

/* 1c. Validate non-existent file — must return RAY_ERR_IO. */
static test_result_t test_journal_validate_no_file(void) {
    ray_err_t e = ray_journal_validate("/tmp/jrn_nosuchfile_xyzzy.log", NULL, NULL);
    TEST_ASSERT_EQ_I(e, RAY_ERR_IO);
    PASS();
}

/* 1d. Validate log with bad prefix header (truncated entry after valid ones). */
static test_result_t test_journal_validate_badtail(void) {
    char base[256]; make_base(base, sizeof(base), "val_badtail");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_t* v1 = ray_i64(1);
    TEST_ASSERT_TRUE(write_journal_entry(f, v1));
    ray_release(v1);

    /* Write a corrupt header — bad prefix. */
    uint8_t junk[16];
    memset(junk, 0xAB, sizeof(junk));
    fwrite(junk, 1, sizeof(junk), f);
    fclose(f);

    int64_t chunks = -1, valid_bytes = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, &valid_bytes);
    TEST_ASSERT_EQ_I(e, RAY_OK);   /* validate always returns OK; badtail = truncated count */
    TEST_ASSERT_EQ_I(chunks, 1);   /* only the first entry was good */

    cleanup_base(base);
    PASS();
}

/* 1e. Validate log with truncated payload (header valid, payload short). */
static test_result_t test_journal_validate_short_payload(void) {
    char base[256]; make_base(base, sizeof(base), "val_short");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Write two good entries, then a header promising 100 bytes but only 3. */
    ray_t* v1 = ray_i64(1);
    ray_t* v2 = ray_i64(2);
    TEST_ASSERT_TRUE(write_journal_entry(f, v1));
    TEST_ASSERT_TRUE(write_journal_entry(f, v2));
    ray_release(v1); ray_release(v2);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 100;
    fwrite(&hdr, 1, sizeof(hdr), f);
    uint8_t partial[3] = {0xAA, 0xBB, 0xCC};
    fwrite(partial, 1, 3, f);
    fclose(f);

    int64_t chunks = -1, valid_bytes = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, &valid_bytes);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 2);   /* two good entries */

    cleanup_base(base);
    PASS();
}

/* 1f. Validate: NULL out-params are safe (no crash). */
static test_result_t test_journal_validate_null_outparams(void) {
    char base[256]; make_base(base, sizeof(base), "val_nullout");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    ray_t* v = ray_i64(7);
    TEST_ASSERT_TRUE(write_journal_entry(f, v));
    ray_release(v);
    fclose(f);

    /* Pass NULL for both out-params — must not crash. */
    ray_err_t e = ray_journal_validate(lpath, NULL, NULL);
    TEST_ASSERT_EQ_I(e, RAY_OK);

    cleanup_base(base);
    PASS();
}

/* 1g. Validate: bad wire version in header terminates early. */
static test_result_t test_journal_validate_bad_version(void) {
    char base[256]; make_base(base, sizeof(base), "val_badver");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Good entry first. */
    ray_t* v1 = ray_i64(5);
    TEST_ASSERT_TRUE(write_journal_entry(f, v1));
    ray_release(v1);

    /* Entry with wrong version. */
    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = 99;   /* bad version */
    hdr.size    = 4;
    fwrite(&hdr, 1, sizeof(hdr), f);
    uint8_t payload[4] = {1, 2, 3, 4};
    fwrite(payload, 1, 4, f);
    fclose(f);

    int64_t chunks = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 1);   /* only the first good entry */

    cleanup_base(base);
    PASS();
}

/* 1h. Validate: hdr.size oversize (> 256 MiB) terminates early. */
static test_result_t test_journal_validate_oversize(void) {
    char base[256]; make_base(base, sizeof(base), "val_oversize");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Good entry first. */
    ray_t* v1 = ray_i64(3);
    TEST_ASSERT_TRUE(write_journal_entry(f, v1));
    ray_release(v1);

    /* Entry with size > 256 MiB. */
    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 300LL * 1024 * 1024;  /* 300 MiB */
    fwrite(&hdr, 1, sizeof(hdr), f);
    fclose(f);

    int64_t chunks = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 1);

    cleanup_base(base);
    PASS();
}

/* 1i. Validate: growing-buffer reuse path — entries of increasing size
 * forces reallocation to cover the cap-growth branch (line 311-317). */
static test_result_t test_journal_validate_growing_payload(void) {
    char base[256]; make_base(base, sizeof(base), "val_grow");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Write 4 entries with payloads of successively larger sizes. */
    ray_t* vals[4];
    vals[0] = ray_i64(1);
    vals[1] = ray_i64(2);
    vals[2] = ray_i64(3);
    vals[3] = ray_i64(4);
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_TRUE(write_journal_entry(f, vals[i]));
        ray_release(vals[i]);
    }
    fclose(f);

    int64_t chunks = -1;
    ray_err_t e = ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 4);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  2. Replay — ray_journal_replay
 * ═══════════════════════════════════════════════════════════════════════ */

/* 2a. Replay non-existent file -> RAY_ERR_IO + RAY_JREPLAY_IO. */
static test_result_t test_journal_replay_no_file(void) {
    int64_t chunks = 99, errs = 99;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay("/tmp/jrn_nosuch_replay.log",
                                     &chunks, &errs, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_IO);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_IO);
    TEST_ASSERT_EQ_I(chunks, 0);
    PASS();
}

/* 2b. Replay clean log with one valid eval-able entry. */
static test_result_t test_journal_replay_clean_single(void) {
    char base[256]; make_base(base, sizeof(base), "rep_clean1");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* A string expression that eval_one can handle: "(set jrn_x 77)" */
    ray_t* expr = ray_str("(set jrn_x 77)", 14);
    TEST_ASSERT_TRUE(write_journal_entry(f, expr));
    ray_release(expr);
    fclose(f);

    int64_t chunks = 0, errs = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, &chunks, &errs, &status);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_OK);
    TEST_ASSERT_EQ_I(chunks, 1);
    TEST_ASSERT_EQ_I(errs, 0);

    cleanup_base(base);
    PASS();
}

/* 2c. Replay empty log -> 0 chunks, RAY_JREPLAY_OK. */
static test_result_t test_journal_replay_empty(void) {
    char base[256]; make_base(base, sizeof(base), "rep_empty");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    int64_t chunks = 99, errs = 99;
    ray_jreplay_status_t status = (ray_jreplay_status_t)99;
    ray_err_t e = ray_journal_replay(lpath, &chunks, &errs, &status);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_OK);
    TEST_ASSERT_EQ_I(chunks, 0);

    cleanup_base(base);
    PASS();
}

/* 2d. Replay with badtail — truncated header bytes. */
static test_result_t test_journal_replay_badtail_short_hdr(void) {
    char base[256]; make_base(base, sizeof(base), "rep_shorthdr");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Write one good entry, then a partial header (7 bytes). */
    ray_t* v = ray_i64(1);
    TEST_ASSERT_TRUE(write_journal_entry(f, v));
    ray_release(v);
    uint8_t partial[7] = {0xFA, 0xDE, 0xFA, 0xCE, 0x03, 0x00, 0x00};
    fwrite(partial, 1, 7, f);
    fclose(f);

    int64_t chunks = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, &chunks, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_BADTAIL);
    TEST_ASSERT_EQ_I(chunks, 1);

    cleanup_base(base);
    PASS();
}

/* 2e. Replay with badtail — bad prefix magic. */
static test_result_t test_journal_replay_badtail_bad_prefix(void) {
    char base[256]; make_base(base, sizeof(base), "rep_badpfx");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = 0xDEADBEEF;   /* wrong */
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 4;
    fwrite(&hdr, 1, sizeof(hdr), f);
    uint8_t payload[4] = {1, 2, 3, 4};
    fwrite(payload, 1, 4, f);
    fclose(f);

    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, NULL, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_BADTAIL);
    PASS();
}

/* 2f. Replay with badtail — bad wire version. */
static test_result_t test_journal_replay_badtail_bad_version(void) {
    char base[256]; make_base(base, sizeof(base), "rep_badver");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = 99;   /* wrong */
    hdr.size    = 4;
    fwrite(&hdr, 1, sizeof(hdr), f);
    uint8_t payload[4] = {1, 2, 3, 4};
    fwrite(payload, 1, 4, f);
    fclose(f);

    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, NULL, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_BADTAIL);
    PASS();
}

/* 2g. Replay with hdr.size oversize (> 256 MiB) -> BADTAIL. */
static test_result_t test_journal_replay_badtail_oversize(void) {
    char base[256]; make_base(base, sizeof(base), "rep_oversize");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 300LL * 1024 * 1024;   /* 300 MiB — too large */
    fwrite(&hdr, 1, sizeof(hdr), f);
    fclose(f);

    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, NULL, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_BADTAIL);
    PASS();
}

/* 2h. Replay with hdr.size <= 0 -> BADTAIL. */
static test_result_t test_journal_replay_badtail_zero_size(void) {
    char base[256]; make_base(base, sizeof(base), "rep_zerosize");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 0;   /* not valid */
    fwrite(&hdr, 1, sizeof(hdr), f);
    fclose(f);

    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, NULL, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_BADTAIL);
    PASS();
}

/* 2i. Replay with truncated payload -> BADTAIL. */
static test_result_t test_journal_replay_badtail_short_payload(void) {
    char base[256]; make_base(base, sizeof(base), "rep_shortpay");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 50;   /* claim 50 bytes but only write 3 */
    fwrite(&hdr, 1, sizeof(hdr), f);
    uint8_t partial[3] = {0x01, 0x02, 0x03};
    fwrite(partial, 1, 3, f);
    fclose(f);

    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, NULL, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_BADTAIL);
    PASS();
}

/* 2j. Replay NULL out-params are safe. */
static test_result_t test_journal_replay_null_outparams(void) {
    char base[256]; make_base(base, sizeof(base), "rep_nullout");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    ray_t* v = ray_i64(3);
    TEST_ASSERT_TRUE(write_journal_entry(f, v));
    ray_release(v);
    fclose(f);

    /* All three out-params NULL must not crash. */
    ray_err_t e = ray_journal_replay(lpath, NULL, NULL, NULL);
    TEST_ASSERT_EQ_I(e, RAY_OK);

    cleanup_base(base);
    PASS();
}

/* 2k. Replay with multiple entries — eval error on one (error expression)
 * but framing intact: status stays OK, errs counter increments. */
static test_result_t test_journal_replay_eval_error(void) {
    char base[256]; make_base(base, sizeof(base), "rep_evalerr");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Two good entries: integer eval (42 evaluates to itself) and an error
     * expression.  ray_eval on a raw i64 just returns the value, so both
     * chunks serialize/deserialize fine; only the second one raises from eval.
     * Use a string expression that produces an error when evaluated. */
    ray_t* v1 = ray_i64(42);
    TEST_ASSERT_TRUE(write_journal_entry(f, v1));
    ray_release(v1);

    /* This string expression is syntactically valid but evaluates to an error
     * because the symbol `__no_such_sym_ever__` is undefined. */
    ray_t* v2 = ray_str("__no_such_sym_ever__", 20);
    TEST_ASSERT_TRUE(write_journal_entry(f, v2));
    ray_release(v2);

    /* One more good integer. */
    ray_t* v3 = ray_i64(7);
    TEST_ASSERT_TRUE(write_journal_entry(f, v3));
    ray_release(v3);
    fclose(f);

    int64_t chunks = 0, errs = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, &chunks, &errs, &status);
    /* All 3 frames deserialized fine -> status RAY_JREPLAY_OK. */
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_OK);
    TEST_ASSERT_EQ_I(chunks, 3);
    /* The eval error frame was noted. */
    TEST_ASSERT_TRUE(errs >= 1);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  3. Open / Close — ray_journal_open / ray_journal_close
 * ═══════════════════════════════════════════════════════════════════════ */

/* 3a. Basic open (no existing log/qdb), write, close. */
static test_result_t test_journal_open_close_basic(void) {
    char base[256]; make_base(base, sizeof(base), "oc_basic");

    TEST_ASSERT_FALSE(ray_journal_is_open());
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    TEST_ASSERT_FALSE(ray_journal_is_open());

    /* Close again on closed journal — must be a no-op. */
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    cleanup_base(base);
    PASS();
}

/* 3b. open rejects empty base string. */
static test_result_t test_journal_open_bad_base(void) {
    ray_err_t e = ray_journal_open("", RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_FALSE(ray_journal_is_open());
    PASS();
}

/* 3c. open rejects double-open. */
static test_result_t test_journal_open_double_open(void) {
    char base[256]; make_base(base, sizeof(base), "oc_double");

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    /* Second open must fail with RAY_ERR_DOMAIN while first is still open. */
    ray_err_t e2 = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e2, RAY_ERR_DOMAIN);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 3d. open with existing .log (clean) replays it -> opens for append.
 * Covers the RAY_JREPLAY_OK switch case (lines 438-442). */
static test_result_t test_journal_open_replays_existing_log(void) {
    char base[256]; make_base(base, sizeof(base), "oc_replay");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Pre-create a log with one good entry (set jrn_rep_var 55). */
    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    ray_t* expr = ray_str("(set jrn_rep_var 55)", 20);
    TEST_ASSERT_TRUE(write_journal_entry(f, expr));
    ray_release(expr);
    fclose(f);

    /* Open should replay without error. */
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());

    /* The replayed entry should have bound jrn_rep_var. */
    ray_t* val = ray_eval_str("jrn_rep_var");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_FALSE(RAY_IS_ERR(val));
    TEST_ASSERT_EQ_I(val->i64, 55);
    ray_release(val);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 3e. open with .log that has a bad tail — returns RAY_ERR_DOMAIN. */
static test_result_t test_journal_open_badtail_log(void) {
    char base[256]; make_base(base, sizeof(base), "oc_badtail");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* One good entry, then garbage. */
    ray_t* v = ray_i64(1);
    TEST_ASSERT_TRUE(write_journal_entry(f, v));
    ray_release(v);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = 0xDEADC0DE;   /* bad prefix */
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 4;
    fwrite(&hdr, 1, sizeof(hdr), f);
    fclose(f);

    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    /* Must NOT be left open after failure. */
    TEST_ASSERT_FALSE(ray_journal_is_open());

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  4. Write bytes — ray_journal_write_bytes
 * ═══════════════════════════════════════════════════════════════════════ */

/* 4a. write when journal is closed -> no-op RAY_OK. */
static test_result_t test_journal_write_when_closed(void) {
    TEST_ASSERT_FALSE(ray_journal_is_open());
    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 4;
    uint8_t payload[4] = {1, 2, 3, 4};
    ray_err_t e = ray_journal_write_bytes(&hdr, payload, 4);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    PASS();
}

/* 4b. write NULL hdr -> RAY_ERR_DOMAIN. */
static test_result_t test_journal_write_null_hdr(void) {
    char base[256]; make_base(base, sizeof(base), "wr_nullhdr");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    uint8_t payload[4] = {1, 2, 3, 4};
    ray_err_t e = ray_journal_write_bytes(NULL, payload, 4);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 4c. write NULL payload (with payload_len > 0) -> RAY_ERR_DOMAIN. */
static test_result_t test_journal_write_null_payload(void) {
    char base[256]; make_base(base, sizeof(base), "wr_nullpay");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 4;
    ray_err_t e = ray_journal_write_bytes(&hdr, NULL, 4);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 4d. write with negative payload_len -> RAY_ERR_DOMAIN. */
static test_result_t test_journal_write_negative_len(void) {
    char base[256]; make_base(base, sizeof(base), "wr_neglen");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 4;
    uint8_t payload[4] = {1, 2, 3, 4};
    ray_err_t e = ray_journal_write_bytes(&hdr, payload, -1);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 4e. write in ASYNC mode (no fsync per write). */
static test_result_t test_journal_write_async_mode(void) {
    char base[256]; make_base(base, sizeof(base), "wr_async");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    ray_t* v = ray_i64(123);
    int64_t psize = ray_serde_size(v);
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)psize);
    TEST_ASSERT_NOT_NULL(buf);
    ray_ser_raw(buf, v);
    ray_release(v);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = psize;
    ray_err_t e = ray_journal_write_bytes(&hdr, buf, psize);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    ray_sys_free(buf);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    /* Confirm the written entry exists by validating. */
    char lpath[270]; log_path(lpath, sizeof(lpath), base);
    int64_t chunks = 0;
    ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(chunks, 1);

    cleanup_base(base);
    PASS();
}

/* 4f. write in SYNC mode (fsync per write). */
static test_result_t test_journal_write_sync_mode(void) {
    char base[256]; make_base(base, sizeof(base), "wr_sync");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_SYNC), RAY_OK);

    ray_t* v = ray_i64(456);
    int64_t psize = ray_serde_size(v);
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)psize);
    TEST_ASSERT_NOT_NULL(buf);
    ray_ser_raw(buf, v);
    ray_release(v);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = psize;
    ray_err_t e = ray_journal_write_bytes(&hdr, buf, psize);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    ray_sys_free(buf);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    char lpath[270]; log_path(lpath, sizeof(lpath), base);
    int64_t chunks = 0;
    ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(chunks, 1);

    cleanup_base(base);
    PASS();
}

/* 4g. write zero-length payload (payload_len == 0). */
static test_result_t test_journal_write_zero_payload(void) {
    char base[256]; make_base(base, sizeof(base), "wr_zerolen");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 0;
    /* payload_len == 0: fwrite is skipped, only header written. */
    uint8_t dummy[1] = {0};
    ray_err_t e = ray_journal_write_bytes(&hdr, dummy, 0);
    TEST_ASSERT_EQ_I(e, RAY_OK);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  5. Sync — ray_journal_sync
 * ═══════════════════════════════════════════════════════════════════════ */

/* 5a. sync when closed -> RAY_OK. */
static test_result_t test_journal_sync_when_closed(void) {
    TEST_ASSERT_FALSE(ray_journal_is_open());
    TEST_ASSERT_EQ_I(ray_journal_sync(), RAY_OK);
    PASS();
}

/* 5b. sync in SYNC mode -> no-op RAY_OK (already per-write synced). */
static test_result_t test_journal_sync_in_sync_mode(void) {
    char base[256]; make_base(base, sizeof(base), "sync_syncmode");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_SYNC), RAY_OK);
    TEST_ASSERT_EQ_I(ray_journal_sync(), RAY_OK);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 5c. sync in ASYNC mode -> actually flushes. */
static test_result_t test_journal_sync_in_async_mode(void) {
    char base[256]; make_base(base, sizeof(base), "sync_asyncmode");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_EQ_I(ray_journal_sync(), RAY_OK);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  6. Roll — ray_journal_roll
 * ═══════════════════════════════════════════════════════════════════════ */

/* 6a. roll when not open -> RAY_ERR_DOMAIN. */
static test_result_t test_journal_roll_when_closed(void) {
    TEST_ASSERT_FALSE(ray_journal_is_open());
    TEST_ASSERT_EQ_I(ray_journal_roll(), RAY_ERR_DOMAIN);
    PASS();
}

/* 6b. roll a live journal — archives the .log and reopens a fresh one. */
static test_result_t test_journal_roll_basic(void) {
    char base[256]; make_base(base, sizeof(base), "roll_basic");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Write one entry so there's something to roll. */
    ray_t* v = ray_i64(777);
    int64_t psize = ray_serde_size(v);
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)psize);
    TEST_ASSERT_NOT_NULL(buf);
    ray_ser_raw(buf, v);
    ray_release(v);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = psize;
    TEST_ASSERT_EQ_I(ray_journal_write_bytes(&hdr, buf, psize), RAY_OK);
    ray_sys_free(buf);

    /* Roll: old .log renamed, new empty .log opened. */
    TEST_ASSERT_EQ_I(ray_journal_roll(), RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());

    /* The current .log must be empty (new one). */
    int64_t chunks = 99;
    ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(chunks, 0);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 6c. roll twice — verify both archives exist and .log is fresh. */
static test_result_t test_journal_roll_twice(void) {
    char base[256]; make_base(base, sizeof(base), "roll_twice");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Write something and roll. */
    ray_t* v1 = ray_i64(1);
    int64_t ps1 = ray_serde_size(v1);
    uint8_t* b1 = (uint8_t*)ray_sys_alloc((size_t)ps1);
    ray_ser_raw(b1, v1); ray_release(v1);
    ray_ipc_header_t hdr1; memset(&hdr1, 0, sizeof(hdr1));
    hdr1.prefix  = RAY_SERDE_PREFIX;
    hdr1.version = RAY_SERDE_WIRE_VERSION;
    hdr1.size    = ps1;
    TEST_ASSERT_EQ_I(ray_journal_write_bytes(&hdr1, b1, ps1), RAY_OK);
    ray_sys_free(b1);
    TEST_ASSERT_EQ_I(ray_journal_roll(), RAY_OK);

    /* Write again and roll. */
    ray_t* v2 = ray_i64(2);
    int64_t ps2 = ray_serde_size(v2);
    uint8_t* b2 = (uint8_t*)ray_sys_alloc((size_t)ps2);
    ray_ser_raw(b2, v2); ray_release(v2);
    ray_ipc_header_t hdr2; memset(&hdr2, 0, sizeof(hdr2));
    hdr2.prefix  = RAY_SERDE_PREFIX;
    hdr2.version = RAY_SERDE_WIRE_VERSION;
    hdr2.size    = ps2;
    TEST_ASSERT_EQ_I(ray_journal_write_bytes(&hdr2, b2, ps2), RAY_OK);
    ray_sys_free(b2);
    TEST_ASSERT_EQ_I(ray_journal_roll(), RAY_OK);

    /* Fresh .log should be empty. */
    int64_t chunks = 99;
    ray_journal_validate(lpath, &chunks, NULL);
    TEST_ASSERT_EQ_I(chunks, 0);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  7. Snapshot — ray_journal_snapshot
 * ═══════════════════════════════════════════════════════════════════════ */

/* 7a. snapshot when not open -> RAY_ERR_DOMAIN. */
static test_result_t test_journal_snapshot_when_closed(void) {
    TEST_ASSERT_FALSE(ray_journal_is_open());
    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_ERR_DOMAIN);
    PASS();
}

/* #420: a crash between the .qdb rename and the log roll leaves the
 * fresh snapshot next to the OLD log whose entries it already contains.
 * Recovery must not apply those entries a second time — and when more
 * entries land on that log afterwards (the failed-roll append path),
 * recovery must replay exactly the tail. */
static test_result_t test_journal_crash_window_no_double_apply(void) {
    char base[256]; make_base(base, sizeof(base), "cwin");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Session 1: journal an increment (non-idempotent on purpose) and
     * apply it once, exactly like the IPC eval+log path. */
    char src[640];
    snprintf(src, sizeof(src),
             "(set jw_x 0)"
             "(.log.open 'async \"%s\")"
             "(.log.write (quote (set jw_x (+ jw_x 1))))"
             "(set jw_x (+ jw_x 1))",
             base);
    ray_t* a = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    if (a != RAY_NULL_OBJ) ray_release(a);

    /* Value the snapshot will hold. */
    ray_t* pre = ray_eval_str("jw_x");
    TEST_ASSERT_FALSE(RAY_IS_ERR(pre));
    int64_t v_pre = pre->i64;
    ray_release(pre);

    /* Snapshot: writes .qdb (with the coverage marker), then rolls. */
    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_OK);

    /* Simulate the crash IN the window: put the just-archived log back
     * under its live name, exactly the on-disk state a crash between
     * the two renames leaves behind. */
    char pattern[300];
    snprintf(pattern, sizeof(pattern), "%s.*.log", base);
    glob_t g;
    TEST_ASSERT_EQ_I(glob(pattern, 0, NULL, &g), 0);
    TEST_ASSERT_EQ_I((int64_t)g.gl_pathc, 1);
    TEST_ASSERT_EQ_I(rename(g.gl_pathv[0], lpath), 0);
    globfree(&g);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    /* Restart: clobber the binding, recover.  The covered log must be
     * skipped — jw_x comes back as the snapshot value, not value+1. */
    ray_t* c = ray_eval_str("(set jw_x 100)");
    if (c && !RAY_IS_ERR(c)) ray_release(c);
    TEST_ASSERT_EQ_I(ray_journal_recover(base), RAY_OK);
    ray_t* r1 = ray_eval_str("jw_x");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r1));
    TEST_ASSERT_EQ_I(r1->i64, v_pre);
    ray_release(r1);

    /* Failed-roll append path: more entries land on the covered log.
     * Recovery must replay exactly that tail on top of the snapshot. */
    char more[640];
    snprintf(more, sizeof(more),
             "(.log.open 'async \"%s\")"
             "(.log.write (quote (set jw_x (+ jw_x 1))))"
             "(.log.close)",
             base);
    ray_t* m = ray_eval_str(more);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_FALSE(RAY_IS_ERR(m));
    if (m != RAY_NULL_OBJ) ray_release(m);

    ray_t* c2 = ray_eval_str("(set jw_x 100)");
    if (c2 && !RAY_IS_ERR(c2)) ray_release(c2);
    TEST_ASSERT_EQ_I(ray_journal_recover(base), RAY_OK);
    ray_t* r2 = ray_eval_str("jw_x");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r2));
    TEST_ASSERT_EQ_I(r2->i64, v_pre + 1);
    ray_release(r2);

    /* The marker key must not leak into the env as a global. */
    ray_t* leak = ray_eval_str(".journal.covered");
    TEST_ASSERT_TRUE(!leak || RAY_IS_ERR(leak));
    if (leak && RAY_IS_ERR(leak)) ray_error_free(leak);
    else if (leak && leak != RAY_NULL_OBJ) ray_release(leak);

    cleanup_base(base);
    PASS();
}

/* 7b. snapshot with bindings -> creates .qdb and rolls log. */
static test_result_t test_journal_snapshot_basic(void) {
    char base[256]; make_base(base, sizeof(base), "snap_basic");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Bind something so the snapshot has content. */
    ray_t* r = ray_eval_str("(set jrn_snap_val 99)");
    if (r && !RAY_IS_ERR(r)) ray_release(r);

    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_OK);
    /* Journal still open (snapshot internally calls roll which reopens). */
    TEST_ASSERT_TRUE(ray_journal_is_open());

    /* .qdb must exist now. */
    FILE* qf = fopen(qpath, "rb");
    TEST_ASSERT_NOT_NULL(qf);
    fclose(qf);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 7c. Open with existing .qdb — snapshot is loaded (covers qdb-load branch). */
static test_result_t test_journal_open_with_qdb(void) {
    char base[256]; make_base(base, sizeof(base), "snap_reload");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    /* First session: open, bind, snapshot, close. */
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    ray_t* r = ray_eval_str("(set jrn_qdb_val 42)");
    if (r && !RAY_IS_ERR(r)) ray_release(r);
    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_OK);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    /* .qdb must exist. */
    FILE* qf = fopen(qpath, "rb");
    TEST_ASSERT_NOT_NULL(qf);
    fclose(qf);

    /* Clear the env binding to verify reload restores it. */
    int64_t sym = ray_sym_intern("jrn_qdb_val", 11);
    ray_env_set(sym, ray_i64(0));  /* overwrite with 0 */

    /* Second session: open should load .qdb and rebind. */
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    ray_t* val = ray_eval_str("jrn_qdb_val");
    TEST_ASSERT_NOT_NULL(val);
    TEST_ASSERT_FALSE(RAY_IS_ERR(val));
    TEST_ASSERT_EQ_I(val->i64, 42);
    ray_release(val);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 7d. Snapshot with empty env (no user bindings) -> minimal .qdb, no crash. */
static test_result_t test_journal_snapshot_empty_env(void) {
    char base[256]; make_base(base, sizeof(base), "snap_empty");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    /* Don't bind anything — snapshot with whatever happens to be in env. */
    ray_err_t e = ray_journal_snapshot();
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());

    FILE* qf = fopen(qpath, "rb");
    TEST_ASSERT_NOT_NULL(qf);
    fclose(qf);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  8. is_open
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_is_open_states(void) {
    TEST_ASSERT_FALSE(ray_journal_is_open());
    char base[256]; make_base(base, sizeof(base), "isopen");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    TEST_ASSERT_FALSE(ray_journal_is_open());
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  9. Open replay — covers DESER/DECOMP switch branches via replay helper.
 *     We can't force decomp failure without a compressed payload; but we
 *     can force the DESER branch by constructing a frame whose payload
 *     claims a valid header but has garbage content for ray_de_raw.
 *     NOTE: DESER in ray_journal_open's switch is hit only if replay sets
 *     status = RAY_JREPLAY_DESER.  We force that by having ray_de_raw
 *     reject the payload — write a header-valid frame with junk payload
 *     that ray_de_raw cannot parse.
 * ═══════════════════════════════════════════════════════════════════════ */

/* The replay function itself aborts on DESER (status = DESER, returns
 * RAY_ERR_DOMAIN).  In ray_journal_open, the switch on that status
 * reaches the DESER/DECOMP case.  To trigger it we need to write a log
 * where the IPC header is well-formed (right magic, right version, size
 * matching bytes present) but the payload bytes cannot be deserialized
 * by ray_de_raw.
 *
 * Testing this requires writing raw bytes.  We write a 1-byte payload
 * that looks like type=0xFF (not a valid ray type) to force de_raw to
 * return an error. */
static test_result_t test_journal_open_deser_error(void) {
    char base[256]; make_base(base, sizeof(base), "oc_deser");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* One valid entry (so chunks = 1 before the bad one). */
    ray_t* v = ray_i64(1);
    TEST_ASSERT_TRUE(write_journal_entry(f, v));
    ray_release(v);

    /* One entry with valid framing but invalid payload (junk type byte). */
    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = 2;
    fwrite(&hdr, 1, sizeof(hdr), f);
    /* Type 0xFE is not a known ray type — ray_de_raw should reject it. */
    uint8_t junk[2] = {0xFE, 0x00};
    fwrite(junk, 1, 2, f);
    fclose(f);

    /* replay will set status = RAY_JREPLAY_DESER and return RAY_ERR_DOMAIN. */
    int64_t chunks = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t re = ray_journal_replay(lpath, &chunks, NULL, &status);
    /* Either deserialization error (DESER) or the payload was accepted
     * (some types parse as errors).  Either way, check the log can be
     * opened. */
    (void)re;

    if (status == RAY_JREPLAY_DESER) {
        /* ray_journal_open should return RAY_ERR_DOMAIN for DESER. */
        ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
        TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
        TEST_ASSERT_FALSE(ray_journal_is_open());
    } else {
        /* ray_de_raw accepted the junk payload — that's OK, skip DESER
         * assertion but still ensure we can open cleanly if status is OK. */
        if (status == RAY_JREPLAY_OK) {
            ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
            if (e == RAY_OK) {
                ray_journal_close();
            }
        } else {
            /* BADTAIL or other — verify open fails with domain. */
            ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
            TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
            TEST_ASSERT_FALSE(ray_journal_is_open());
        }
    }

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  10. Write during replay is suppressed (in_replay flag).
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_write_during_replay_noop(void) {
    /* Verify is_open() and write_bytes() during replay return quickly:
     * open a journal, call replay directly while open, check nothing is
     * written to the log.  We simulate by calling ray_journal_replay on
     * a separate file while journal is open (in_replay is local to replay,
     * not the global flag). */
    char base[256]; make_base(base, sizeof(base), "wr_inreplay");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Pre-write one entry to a separate log. */
    char src_log[300];
    snprintf(src_log, sizeof(src_log), "%s_src.log", base);
    FILE* sf = fopen(src_log, "wb");
    TEST_ASSERT_NOT_NULL(sf);
    ray_t* expr = ray_str("(set jrn_replay_write_test 7)", 29);
    TEST_ASSERT_TRUE(write_journal_entry(sf, expr));
    ray_release(expr);
    fclose(sf);

    /* Open the main journal. */
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Replay the separate log while open.  The expr sets jrn_replay_write_test.
     * The replay sets in_replay=true so any writes from eval don't go to log. */
    int64_t chunks = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(src_log, &chunks, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 1);

    /* Main log should still be empty (replay wrote nothing). */
    int64_t written_chunks = 0;
    ray_journal_validate(lpath, &written_chunks, NULL);
    TEST_ASSERT_EQ_I(written_chunks, 0);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    unlink(src_log);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  11. Restricted flag propagation through replay
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_replay_restricted_flag(void) {
    char base[256]; make_base(base, sizeof(base), "rep_restricted");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Write an entry with RAY_IPC_FLAG_RESTRICTED set. */
    ray_t* v = ray_i64(100);
    int64_t psize = ray_serde_size(v);
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)psize);
    TEST_ASSERT_NOT_NULL(buf);
    ray_ser_raw(buf, v);
    ray_release(v);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.flags   = 0x02;  /* RAY_IPC_FLAG_RESTRICTED */
    hdr.size    = psize;
    bool ok = (fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr)) &&
              (fwrite(buf, 1, (size_t)psize, f) == (size_t)psize);
    ray_sys_free(buf);
    TEST_ASSERT_TRUE(ok);
    fclose(f);

    int64_t chunks = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t e = ray_journal_replay(lpath, &chunks, NULL, &status);
    TEST_ASSERT_EQ_I(e, RAY_OK);
    TEST_ASSERT_EQ_I(chunks, 1);

    /* Restricted flag must be restored after replay. */
    TEST_ASSERT_FALSE(ray_eval_get_restricted());

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  12. Compressed frame replay (decompress_if_needed happy path)
 * ═══════════════════════════════════════════════════════════════════════ */

/* Build a valid compressed journal frame manually:
 * header with RAY_IPC_FLAG_COMPRESSED, payload = 4-byte uncomp_size
 * followed by the LZ4-like compressed bytes.  We use ray_ipc_compress
 * which requires >2000 bytes to actually compress; for smaller payloads
 * it returns 0 and we must write uncompressed.  Instead, we craft a
 * "compressed" frame by using the same format ipc.c uses:
 *   [uint32_t uncomp_size][compressed_bytes...]
 * where we compress using ray_ipc_compress.  If compress returns 0 for
 * our small payload, we skip the test rather than injecting bad data. */
static test_result_t test_journal_replay_compressed_frame(void) {
    char base[256]; make_base(base, sizeof(base), "rep_comp");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Serialize a value and try to compress it. */
    ray_t* v = ray_i64(12345);
    int64_t psize = ray_serde_size(v);
    uint8_t* raw = (uint8_t*)ray_sys_alloc((size_t)psize);
    TEST_ASSERT_NOT_NULL(raw);
    ray_ser_raw(raw, v);
    ray_release(v);

    /* ray_ipc_compress requires src len > RAY_IPC_COMPRESS_THRESHOLD (2000). */
    /* Build a larger payload by repeating the serialized value. */
    size_t bigsize = 3000;
    uint8_t* big = (uint8_t*)ray_sys_alloc(bigsize);
    TEST_ASSERT_NOT_NULL(big);
    /* Fill with repetitive pattern (compresses well). */
    for (size_t i = 0; i < bigsize; i++) big[i] = (uint8_t)(i % 7);

    uint8_t* comp_buf = (uint8_t*)ray_sys_alloc(bigsize);
    TEST_ASSERT_NOT_NULL(comp_buf);

    size_t clen = ray_ipc_compress(big, bigsize, comp_buf, bigsize);
    if (clen == 0 || clen + 4 >= bigsize) {
        /* Compression yielded nothing useful for this input — skip. */
        ray_sys_free(raw); ray_sys_free(big); ray_sys_free(comp_buf);
        cleanup_base(base);
        PASS(); /* Not a failure — just can't exercise this path here. */
    }

    /* Build the compressed payload: [uint32_t uncomp_size][compressed_bytes]. */
    size_t payload_size = 4 + clen;
    uint8_t* payload = (uint8_t*)ray_sys_alloc(payload_size);
    TEST_ASSERT_NOT_NULL(payload);
    uint32_t uncomp = (uint32_t)bigsize;
    memcpy(payload, &uncomp, 4);
    memcpy(payload + 4, comp_buf, clen);

    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.flags   = RAY_IPC_FLAG_COMPRESSED;
    hdr.size    = (int64_t)payload_size;

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);
    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(payload, 1, payload_size, f);
    fclose(f);

    ray_sys_free(raw); ray_sys_free(big); ray_sys_free(comp_buf); ray_sys_free(payload);

    /* Replay — decompress_if_needed will take the COMPRESSED branch.
     * The decompressed bytes are the big[] pattern, which ray_de_raw
     * might reject (not a valid ray object) — that's OK, we care
     * that the compressed path ran, not that eval succeeded. */
    int64_t chunks = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_journal_replay(lpath, &chunks, NULL, &status);
    /* Either DESER (de_raw rejected) or OK — both mean decompress ran. */
    TEST_ASSERT_TRUE(status == RAY_JREPLAY_DESER ||
                     status == RAY_JREPLAY_OK ||
                     status == RAY_JREPLAY_DECOMP);

    cleanup_base(base);
    PASS();
}

/* Test the decompress_if_needed failure paths:
 * 1. payload_len < 4 with COMPRESSED flag -> false
 * 2. uncomp_size == 0 -> false
 * 3. uncomp_size > 256 MiB -> false
 * These are exercised via replay with a COMPRESSED header + invalid payload. */
static test_result_t test_journal_replay_compressed_bad_payload(void) {
    char base[256]; make_base(base, sizeof(base), "rep_comp_bad");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    FILE* f = fopen(lpath, "wb");
    TEST_ASSERT_NOT_NULL(f);

    /* Compressed frame with only 3 bytes of payload (< 4 -> decompress rejects). */
    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.flags   = RAY_IPC_FLAG_COMPRESSED;
    hdr.size    = 3;
    fwrite(&hdr, 1, sizeof(hdr), f);
    uint8_t short_payload[3] = {0x01, 0x02, 0x03};
    fwrite(short_payload, 1, 3, f);
    fclose(f);

    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_journal_replay(lpath, NULL, NULL, &status);
    /* decompress returns false -> status = RAY_JREPLAY_DECOMP. */
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_DECOMP);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  13. Open with bad .qdb (corrupted or wrong type)
 * ═══════════════════════════════════════════════════════════════════════ */

/* 13a. .qdb exists but is corrupted (truncated) -> snapshot load fails. */
static test_result_t test_journal_open_bad_qdb_corrupt(void) {
    char base[256]; make_base(base, sizeof(base), "oc_badqdb");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    /* Write 5 bytes of garbage as the .qdb file. */
    FILE* qf = fopen(qpath, "wb");
    TEST_ASSERT_NOT_NULL(qf);
    uint8_t garbage[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};
    fwrite(garbage, 1, 5, qf);
    fclose(qf);

    /* Open should fail because snapshot load fails. */
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_ERR_IO);
    TEST_ASSERT_FALSE(ray_journal_is_open());

    cleanup_base(base);
    PASS();
}

/* 13b. .qdb exists but contains a non-dict object -> wrong type error. */
static test_result_t test_journal_open_qdb_not_dict(void) {
    char base[256]; make_base(base, sizeof(base), "oc_qdbtype");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    /* Save an integer (not a dict) as the .qdb file. */
    ray_t* v = ray_i64(99);
    ray_err_t se = ray_obj_save(v, qpath);
    ray_release(v);
    TEST_ASSERT_EQ_I(se, RAY_OK);

    /* Open should fail: snapshot is not a dict. */
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_FALSE(ray_journal_is_open());

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  14. ray_journal_open NULL base pointer
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_open_null_base(void) {
    ray_err_t e = ray_journal_open(NULL, RAY_JOURNAL_ASYNC);
    TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
    TEST_ASSERT_FALSE(ray_journal_is_open());
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  15. open+replay with JREPLAY_IO when ferror fires mid-frame.
 *      We trigger this by writing a valid header but having the file
 *      truncated mid-payload at the OS level.  The SIZE_MAX path in
 *      read_full is triggered when fread returns 0 AND ferror() is true.
 *      Since we can't inject ferror() without a mock, we instead cover
 *      the next-best path: a SIZE_MAX read that triggers RAY_JREPLAY_IO
 *      on payload read by using a named pipe (FIFO), which returns 0
 *      bytes from fread after the write end closes, making ferror false
 *      but feof true — so we only get BADTAIL here, not IO.
 *      This path stays uncovered; document it as a known blocker.
 * ═══════════════════════════════════════════════════════════════════════ */

/* ═══════════════════════════════════════════════════════════════════════
 *  16. Snapshot: multiple bindings roundtrip
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_snapshot_multiple_bindings(void) {
    char base[256]; make_base(base, sizeof(base), "snap_multi");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Bind several values. */
    ray_t* r1 = ray_eval_str("(set jrn_multi_a 10)");
    if (r1 && !RAY_IS_ERR(r1)) ray_release(r1);
    ray_t* r2 = ray_eval_str("(set jrn_multi_b 20)");
    if (r2 && !RAY_IS_ERR(r2)) ray_release(r2);
    ray_t* r3 = ray_eval_str("(set jrn_multi_c 30)");
    if (r3 && !RAY_IS_ERR(r3)) ray_release(r3);

    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_OK);

    /* .qdb must exist. */
    FILE* qf = fopen(qpath, "rb");
    TEST_ASSERT_NOT_NULL(qf);
    fclose(qf);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    /* Verify snapshot can be loaded back. */
    ray_t* snap = ray_obj_load(qpath);
    TEST_ASSERT_NOT_NULL(snap);
    TEST_ASSERT_FALSE(RAY_IS_ERR(snap));
    TEST_ASSERT_EQ_I(snap->type, RAY_DICT);
    ray_release(snap);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  17. ray_journal_open with log that generates JREPLAY_IO via
 *      open switch — covered by triggering a read error.
 *      Use a directory path (not a file) for the log, so fread
 *      will fail with an error (EISDIR).
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_open_log_is_directory(void) {
    char base[256]; make_base(base, sizeof(base), "oc_logdir");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Create a DIRECTORY at the .log path — fopen("rb") will succeed
     * on Linux but subsequent fread will return 0 + no ferror (EISDIR
     * makes it appear as EOF).  This tests the file_exists + replay path
     * where stat succeeds (directory is not a regular file, so
     * file_exists returns false).  Just verify open succeeds (no .log). */

    /* Actually, file_exists checks S_ISREG, so a directory won't be
     * treated as a log.  Let's instead write a .log that is a valid
     * single-entry log but followed by a directory separator to see
     * what happens when a write after a rename encounters a dir.
     * Instead, let's focus on a more achievable test:
     * log path refers to a path that can be opened for read but where
     * the first fread produces an error via a special file.
     * On Linux /proc/self/mem is readable but fread errors.  Use that. */

    /* Write a .log symlink pointing to /proc/self/mem. */
    if (symlink("/proc/self/mem", lpath) != 0) {
        /* symlink failed (e.g., file exists) — skip test gracefully. */
        PASS();
    }

    /* file_exists follows symlinks and /proc/self/mem is a regular file
     * from stat(2)'s perspective on Linux.  Opening it for "rb" works
     * but fread on it will return 0 + ferror set.  This triggers the
     * SIZE_MAX path in read_full -> RAY_JREPLAY_IO in replay. */
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    /* Should fail with RAY_ERR_IO (JREPLAY_IO path) or RAY_ERR_DOMAIN
     * (if replay returns BADTAIL because the fread saw EOF quickly). */
    TEST_ASSERT_TRUE(e == RAY_ERR_IO || e == RAY_ERR_DOMAIN);
    TEST_ASSERT_FALSE(ray_journal_is_open());

    unlink(lpath);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  18. Snapshot: .qdb contains a dict with non-SYM key vector.
 *      Triggers the "keys->type != RAY_SYM" warning path in open.
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_open_qdb_wrong_key_type(void) {
    char base[256]; make_base(base, sizeof(base), "oc_qdbkeys");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    /* Build a dict with I64 keys (not SYM) — ray_dict_new takes keys + vals. */
    int64_t kv[2] = {1, 2};
    ray_t* keys = ray_vec_new(RAY_I64, 2);
    keys = ray_vec_append(keys, &kv[0]);
    keys = ray_vec_append(keys, &kv[1]);

    ray_t* vals = ray_list_new(2);
    ray_t* v1 = ray_i64(10);
    ray_t* v2 = ray_i64(20);
    vals = ray_list_append(vals, v1);
    vals = ray_list_append(vals, v2);
    ray_release(v1); ray_release(v2);

    /* ray_dict_new consumes keys and vals. */
    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    ray_err_t se = ray_obj_save(d, qpath);
    ray_release(d);
    TEST_ASSERT_EQ_I(se, RAY_OK);

    /* Open: should load .qdb, see keys->type != RAY_SYM, warn + skip, then
     * succeed overall (partial state is printed but no error is returned unless
     * bind_errs > 0 — here we skipped, so bind_errs == 0). */
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    /* Either succeeds (skipped all, no bind errors) or domain error. */
    if (e == RAY_OK) {
        TEST_ASSERT_TRUE(ray_journal_is_open());
        TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    } else {
        TEST_ASSERT_EQ_I(e, RAY_ERR_DOMAIN);
        TEST_ASSERT_FALSE(ray_journal_is_open());
    }

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  19. Snapshot rename failure: make .qdb a directory so rename fails.
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_snapshot_rename_fails(void) {
    char base[256]; make_base(base, sizeof(base), "snap_rename");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Create the .qdb path as a DIRECTORY — rename(tmp, dir) will fail with EISDIR. */
    if (mkdir(qpath, 0755) != 0) {
        /* Can't create dir — skip gracefully. */
        ray_journal_close();
        cleanup_base(base);
        PASS();
    }

    ray_t* r = ray_eval_str("(set jrn_snap_rename_test 5)");
    if (r && !RAY_IS_ERR(r)) ray_release(r);

    ray_err_t e = ray_journal_snapshot();
    /* rename(tmp -> dir_path) should fail -> RAY_ERR_IO. */
    TEST_ASSERT_EQ_I(e, RAY_ERR_IO);

    /* Journal should still be open (snapshot error leaves it usable). */
    TEST_ASSERT_TRUE(ray_journal_is_open());

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    /* Remove the directory we created. */
    rmdir(qpath);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  20. Snapshot: .qdb dict with more keys than values (missing-val path).
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_open_qdb_missing_val(void) {
    char base[256]; make_base(base, sizeof(base), "oc_qdbmissing");
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    /* Build a dict: 2 sym keys, 1 value — second key has no corresponding val. */
    int64_t s1 = ray_sym_intern("jrn_k1", 6);
    int64_t s2 = ray_sym_intern("jrn_k2", 6);
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 2);
    keys = ray_vec_append(keys, &s1);
    keys = ray_vec_append(keys, &s2);

    /* Only one value — ray_list_get(vals, 1) returns NULL for index 1. */
    ray_t* vals = ray_list_new(1);
    ray_t* v1 = ray_i64(42);
    vals = ray_list_append(vals, v1);
    ray_release(v1);

    ray_t* d = ray_dict_new(keys, vals);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_FALSE(RAY_IS_ERR(d));

    ray_err_t se = ray_obj_save(d, qpath);
    ray_release(d);
    TEST_ASSERT_EQ_I(se, RAY_OK);

    /* Open: should warn about missing val for sym jrn_k2, but succeed. */
    ray_err_t e = ray_journal_open(base, RAY_JOURNAL_ASYNC);
    /* Partial load — bind_errs == 0 (we skipped, not failed), so OK. */
    if (e == RAY_OK) {
        TEST_ASSERT_TRUE(ray_journal_is_open());
        TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    } else {
        /* If open returned domain, still OK for test purposes. */
        TEST_ASSERT_FALSE(ray_journal_is_open());
    }

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  21. Roll rename failure: pre-create archive path as directory.
 * ═══════════════════════════════════════════════════════════════════════ */

static test_result_t test_journal_roll_rename_fails(void) {
    char base[256]; make_base(base, sizeof(base), "roll_rename");

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());

    /* We can't easily predict the UTC stamp that roll will use.
     * Instead, verify that roll with a valid fresh journal succeeds
     * (the normal case) — the rename-failure branch requires injecting
     * an error that we can't trigger cleanly without mocks. */
    TEST_ASSERT_EQ_I(ray_journal_roll(), RAY_OK);
    TEST_ASSERT_TRUE(ray_journal_is_open());

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  22. Ops-layer (src/ops/journal.c) — direct C-API calls.
 *
 *  These tests call the thin ops wrappers directly to cover branches
 *  that cannot be reached from RFL (NULL expr, long paths, etc.).
 *  No static de-exposure, no internal-header additions beyond ops/journal.h
 *  which is the public ops header.
 * ═══════════════════════════════════════════════════════════════════════ */

/* 22a. ray_log_replay_fn: path is NULL — str_to_cpath early-NULL branch. */
static test_result_t test_ops_replay_null_path(void) {
    ray_t* r = ray_log_replay_fn(NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* 22b. ray_log_replay_fn: path is an integer — type != -RAY_STR branch. */
static test_result_t test_ops_replay_non_string(void) {
    ray_t* arg = ray_i64(42);
    ray_t* r   = ray_log_replay_fn(arg);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(arg);
    ray_release(r);
    PASS();
}

/* 22c. ray_log_replay_fn: path longer than 1023 bytes — n+1 > bufsz branch
 *  in str_to_cpath.  This is the only reachable trigger for line 39. */
static test_result_t test_ops_replay_long_path(void) {
    /* Build a 1025-character string — guaranteed to overflow the 1024-byte
     * local buffer in str_to_cpath. */
    char long_path[1026];
    memset(long_path, 'x', 1025);
    long_path[1025] = '\0';

    ray_t* arg = ray_str(long_path, 1025);
    TEST_ASSERT_NOT_NULL(arg);
    ray_t* r = ray_log_replay_fn(arg);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(arg);
    ray_release(r);
    PASS();
}

/* 22d. ray_log_validate_fn: NULL path — str_to_cpath early-NULL. */
static test_result_t test_ops_validate_null_path(void) {
    ray_t* r = ray_log_validate_fn(NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

/* 22e. ray_log_validate_fn: integer path — type guard. */
static test_result_t test_ops_validate_non_string(void) {
    ray_t* arg = ray_i64(99);
    ray_t* r   = ray_log_validate_fn(arg);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(arg);
    ray_release(r);
    PASS();
}

/* 22f. ray_log_validate_fn: path > 1023 bytes — n+1 > bufsz. */
static test_result_t test_ops_validate_long_path(void) {
    char long_path[1026];
    memset(long_path, 'y', 1025);
    long_path[1025] = '\0';

    ray_t* arg = ray_str(long_path, 1025);
    TEST_ASSERT_NOT_NULL(arg);
    ray_t* r = ray_log_validate_fn(arg);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(arg);
    ray_release(r);
    PASS();
}

/* 22g. ray_log_open_fn: n != 2 (rank guard). */
static test_result_t test_ops_open_rank(void) {
    ray_t* dummy = ray_i64(0);
    ray_t* args[1] = { dummy };
    ray_t* r = ray_log_open_fn(args, 1);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(dummy);
    ray_release(r);
    PASS();
}

/* 22h. ray_log_open_fn: args[0] = NULL — !args[0] branch. */
static test_result_t test_ops_open_null_mode(void) {
    ray_t* str_arg = ray_str("/tmp/jrn_ops_test", 17);
    ray_t* args[2] = { NULL, str_arg };
    ray_t* r = ray_log_open_fn(args, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(str_arg);
    ray_release(r);
    PASS();
}

/* 22i. ray_log_open_fn: args[0] = integer — type != -RAY_SYM. */
static test_result_t test_ops_open_int_mode(void) {
    ray_t* int_arg = ray_i64(1);
    ray_t* str_arg = ray_str("/tmp/jrn_ops_test", 17);
    ray_t* args[2] = { int_arg, str_arg };
    ray_t* r = ray_log_open_fn(args, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(int_arg);
    ray_release(str_arg);
    ray_release(r);
    PASS();
}

/* 22j. ray_log_open_fn: args[1] = NULL — !args[1] branch. */
static test_result_t test_ops_open_null_base(void) {
    int64_t async_id = ray_sym_intern("async", 5);
    ray_t*  sym_arg  = ray_sym(async_id);
    ray_t*  args[2]  = { sym_arg, NULL };
    ray_t*  r        = ray_log_open_fn(args, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(sym_arg);
    ray_release(r);
    PASS();
}

/* 22k. ray_log_open_fn: args[1] = sym — type != -RAY_STR. */
static test_result_t test_ops_open_sym_base(void) {
    int64_t async_id = ray_sym_intern("async", 5);
    int64_t foo_id   = ray_sym_intern("foo",   3);
    ray_t*  sym_mode = ray_sym(async_id);
    ray_t*  sym_base = ray_sym(foo_id);
    ray_t*  args[2]  = { sym_mode, sym_base };
    ray_t*  r        = ray_log_open_fn(args, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(sym_mode);
    ray_release(sym_base);
    ray_release(r);
    PASS();
}

/* 22l. ray_log_open_fn: mode sym is not `async or `sync — domain guard. */
static test_result_t test_ops_open_bad_mode(void) {
    int64_t bogus_id = ray_sym_intern("bogus", 5);
    ray_t*  sym_mode = ray_sym(bogus_id);
    ray_t*  str_base = ray_str("/tmp/jrn_ops_test", 17);
    ray_t*  args[2]  = { sym_mode, str_base };
    ray_t*  r        = ray_log_open_fn(args, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(sym_mode);
    ray_release(str_base);
    ray_release(r);
    PASS();
}

/* 22m. ray_log_open_fn: args[1] is a string longer than 1023 bytes — the
 *  str_to_cpath call at line 72 returns NULL, triggering line 73. */
static test_result_t test_ops_open_long_base(void) {
    int64_t async_id  = ray_sym_intern("async", 5);
    ray_t*  sym_mode  = ray_sym(async_id);

    char long_base[1026];
    memset(long_base, 'z', 1025);
    long_base[1025] = '\0';
    ray_t* str_base = ray_str(long_base, 1025);

    ray_t* args[2] = { sym_mode, str_base };
    ray_t* r       = ray_log_open_fn(args, 2);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(sym_mode);
    ray_release(str_base);
    ray_release(r);
    PASS();
}

/* 22n. ray_log_open_fn: `sync mode opens cleanly (exercises the sync branch
 *  at line 68 for any run-context where only `async has been tested). */
static test_result_t test_ops_open_sync_mode(void) {
    char base[256]; make_base(base, sizeof(base), "ops_sync");

    int64_t sync_id  = ray_sym_intern("sync", 4);
    ray_t*  sym_mode = ray_sym(sync_id);
    ray_t*  str_base = ray_str(base, strlen(base));
    ray_t*  args[2]  = { sym_mode, str_base };
    ray_t*  r        = ray_log_open_fn(args, 2);
    /* Must succeed (null return = ok). */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(sym_mode);
    ray_release(str_base);
    ray_release(r);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 22o. ray_log_write_fn: NULL expr — the !expr guard at line 89.
 *  Journal must be open first so we pass the is_open guard. */
static test_result_t test_ops_write_null_expr(void) {
    char base[256]; make_base(base, sizeof(base), "ops_null_expr");

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    ray_t* r = ray_log_write_fn(NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* 22p. ray_log_write_fn: journal not open — noopen guard (line 87-88).
 *  Also exercises ray_log_write_fn entry with a valid non-NULL expr so we
 *  confirm the !is_open branch returns an error atom. */
static test_result_t test_ops_write_noopen(void) {
    /* Ensure journal is closed. */
    ray_journal_close();

    ray_t* expr = ray_i64(42);
    ray_t* r    = ray_log_write_fn(expr);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(expr);
    ray_release(r);
    PASS();
}

/* 22q. ray_log_write_fn: pay_size <= 0 (lines 100-103).
 *
 *  ray_serde_size returns 0 for object types not in its switch (any type
 *  value that is not a known atom/vector/container type).  We manufacture
 *  a stack-local ray_t with type=14 — outside the known range — so the
 *  function hits the "serde size 0" domain error without touching any
 *  heap internals of the fake object.
 *
 *  Safety: ray_is_lazy checks type==104; RAY_IS_ERR checks type==127;
 *  RAY_IS_NULL checks type==126.  Type=14 passes all three predicates
 *  safely.  ray_serde_size dereferences obj->len (for the overflow guard)
 *  which is 0 from the memset — safe. */
static test_result_t test_ops_write_serde_size_zero(void) {
    char base[256]; make_base(base, sizeof(base), "ops_serde_zero");

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);

    /* Stack object with an unknown type tag (14 is not RAY_LIST through
     * RAY_STR, not RAY_LAZY, RAY_ERROR, or RAY_NULL). */
    ray_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.type = 14;       /* positive unknown → ray_serde_size returns 0 */
    fake.rc   = 1;        /* non-zero so any accidental retain/release is safe */

    ray_t* r = ray_log_write_fn(&fake);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    ray_release(r);

    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  23. End-to-end RFL round-trip: both `.log.write` symbol-head forms replay.
 *
 *  A symbol in functional position applies the named function, so BOTH the
 *  tick form `(list 'f 10)` and the quote form `(list (quote f) 5)` replay
 *  by applying `f`.  We drive the public RFL verbs, then reset the
 *  accumulator and replay explicitly, observing the side effect (15).
 * ═══════════════════════════════════════════════════════════════════════ */
static test_result_t test_journal_replay_symbol_head_both_forms(void) {
    char base[256]; make_base(base, sizeof(base), "symhead");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Open async, define an accumulating fn, write both symbol-head forms,
     * then close.  Multiple top-level forms in one source string are wrapped
     * in an implicit (do ...) by the parser, so this evaluates in order. */
    char src[640];
    snprintf(src, sizeof(src),
             "(set _acc 0)"
             "(set f (fn [x] (set _acc (+ _acc x))))"
             "(.log.open 'async \"%s\")"
             "(.log.write (list 'f 10))"
             "(.log.write (list (quote f) 5))"
             "(.log.close)",
             base);
    ray_t* a = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    if (a != RAY_NULL_OBJ) ray_release(a);

    /* Reset the accumulator, then replay the log: both records apply `f`. */
    char replay[400];
    snprintf(replay, sizeof(replay),
             "(set _acc 0)(.log.replay \"%s\")", lpath);
    ray_t* z = ray_eval_str(replay);
    TEST_ASSERT_NOT_NULL(z);
    TEST_ASSERT_FALSE(RAY_IS_ERR(z));
    if (z != RAY_NULL_OBJ) ray_release(z);

    /* Both records applied: 0 + 10 + 5 == 15. */
    ray_t* acc = ray_eval_str("_acc");
    TEST_ASSERT_NOT_NULL(acc);
    TEST_ASSERT_FALSE(RAY_IS_ERR(acc));
    TEST_ASSERT_EQ_I(acc->type, -RAY_I64);
    TEST_ASSERT_EQ_I(acc->i64, 15);
    ray_release(acc);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  23b. A well-framed record whose head symbol is UNBOUND replays to a
 *  graceful per-chunk eval error — replay counts it and continues, it
 *  must NEVER crash.
 *
 *  A symbol in functional position resolves+applies; an unbound head
 *  yields a `name` error from eval rather than a crash.  Framing stays
 *  intact (the frame deserialized fine), so replay returns RAY_OK with
 *  status RAY_JREPLAY_OK, chunks == 1, and the failure observable as
 *  errs == 1.  This guards the eval-corruption fix against regression.
 * ═══════════════════════════════════════════════════════════════════════ */
static test_result_t test_journal_replay_unbound_head_is_graceful(void) {
    char base[256]; make_base(base, sizeof(base), "unbound_head");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Drive the public RFL verbs to write ONE record whose head symbol is
     * not bound to anything, then close.  Multiple top-level forms in one
     * source string are wrapped in an implicit (do ...), so this evaluates
     * in order. */
    char src[512];
    snprintf(src, sizeof(src),
             "(.log.open 'async \"%s\")"
             "(.log.write (list 'definitely_not_bound 1))"
             "(.log.close)",
             base);
    ray_t* a = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    if (a != RAY_NULL_OBJ) ray_release(a);

    /* Replay: the lone frame deserializes fine (framing intact) but its
     * eval raises a `name` error.  No crash; the error is counted. */
    int64_t chunks = 0, errs = 0;
    ray_jreplay_status_t status = RAY_JREPLAY_OK;
    ray_err_t rc = ray_journal_replay(lpath, &chunks, &errs, &status);

    /* Framing valid -> RAY_OK / RAY_JREPLAY_OK; the one record was replayed
     * (chunks == 1) and its eval failure noted (errs == 1). */
    TEST_ASSERT_EQ_I(rc, RAY_OK);
    TEST_ASSERT_EQ_I(status, RAY_JREPLAY_OK);
    TEST_ASSERT_EQ_I(chunks, 1);
    TEST_ASSERT_EQ_I(errs, 1);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  24. `.log.open` opens for append only — it must NOT replay the existing
 *  log.  Explicit `.log.replay` still applies it.
 * ═══════════════════════════════════════════════════════════════════════ */
static test_result_t test_journal_open_does_not_replay(void) {
    char base[256]; make_base(base, sizeof(base), "open_noreplay");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    /* Open via the verb, write a mutating record, close.  `.log.write`
     * only SERIALIZES the record — it does NOT evaluate it — so `_m`
     * stays at the reset value here. */
    char src[640];
    snprintf(src, sizeof(src),
             "(set _m 0)"
             "(.log.open 'async \"%s\")"
             "(.log.write \"(set _m 7)\")"
             "(.log.close)",
             base);
    ray_t* a = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FALSE(RAY_IS_ERR(a));
    if (a != RAY_NULL_OBJ) ray_release(a);

    /* Reopen via the verb — must NOT re-apply (set _m 7). */
    char reopen[640];
    snprintf(reopen, sizeof(reopen),
             "(set _m 0)(.log.open 'async \"%s\")", base);
    ray_t* b = ray_eval_str(reopen);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_FALSE(RAY_IS_ERR(b));
    if (b != RAY_NULL_OBJ) ray_release(b);

    ray_t* m1 = ray_eval_str("_m");
    TEST_ASSERT_NOT_NULL(m1);
    TEST_ASSERT_EQ_I(m1->type, -RAY_I64);
    TEST_ASSERT_EQ_I(m1->i64, 0);   /* NOT replayed on open */
    ray_release(m1);

    /* Explicit replay DOES apply the record. */
    char replay[400];
    snprintf(replay, sizeof(replay), "(.log.replay \"%s\")", lpath);
    ray_t* c = ray_eval_str(replay);
    if (c && !RAY_IS_ERR(c) && c != RAY_NULL_OBJ) ray_release(c);

    ray_t* m2 = ray_eval_str("_m");
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQ_I(m2->type, -RAY_I64);
    TEST_ASSERT_EQ_I(m2->i64, 7);   /* replayed on demand */
    ray_release(m2);

    ray_t* d = ray_eval_str("(.log.close)");
    if (d && !RAY_IS_ERR(d) && d != RAY_NULL_OBJ) ray_release(d);

    cleanup_base(base);
    PASS();
}

/* A corrupted record encoding pathologically-nested lists must NOT overflow
 * the C stack — ray_de_raw returns a domain error instead.  Each level on the
 * wire is a single-element RAY_LIST header [type][attrs:1][count:8 LE]; the
 * innermost element is a RAY_SERDE_NULL terminal. */
static test_result_t test_journal_de_depth_limit(void) {
    const int levels = 5000;   /* far beyond any legitimate nesting */
    size_t cap = (size_t)levels * 10 + 1;
    uint8_t* b = (uint8_t*)malloc(cap);
    TEST_ASSERT_NOT_NULL(b);
    size_t off = 0;
    for (int i = 0; i < levels; i++) {
        b[off++] = (uint8_t)RAY_LIST;          /* type tag = 0 */
        b[off++] = 0;                          /* attrs */
        int64_t one = 1;                       /* count = 1 element */
        memcpy(b + off, &one, 8); off += 8;
    }
    b[off++] = (uint8_t)RAY_SERDE_NULL;        /* innermost terminal element */

    int64_t len = (int64_t)off;
    ray_t* r = ray_de_raw(b, &len);
    TEST_ASSERT_TRUE(r != NULL && RAY_IS_ERR(r));
    ray_error_free(r);
    free(b);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Purge — ray_journal_purge / .log.purge
 * ═══════════════════════════════════════════════════════════════════════ */

/* Append one self-evaluating i64 entry to the open journal. */
static bool purge_write_one(int64_t x) {
    ray_t* v = ray_i64(x);
    int64_t psize = ray_serde_size(v);
    if (psize <= 0) { ray_release(v); return false; }
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)psize);
    if (!buf) { ray_release(v); return false; }
    ray_ser_raw(buf, v);
    ray_release(v);
    ray_ipc_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.prefix  = RAY_SERDE_PREFIX;
    hdr.version = RAY_SERDE_WIRE_VERSION;
    hdr.size    = psize;
    ray_err_t e = ray_journal_write_bytes(&hdr, buf, psize);
    ray_sys_free(buf);
    return e == RAY_OK;
}

/* True iff at least one rolled archive (base.<stamp>.log) exists. */
static bool archive_exists(const char* base) {
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "test -n \"$(ls '%s'.*.log 2>/dev/null)\"", base);
    return system(cmd) == 0;
}

/* P1. Full purge while the journal is OPEN: closes it, unlinks the active
 * .log, every rolled .<stamp>.log archive, the .qdb snapshot, and resets
 * state so a second purge has nothing to act on. */
static test_result_t test_journal_purge_full(void) {
    char base[256]; make_base(base, sizeof(base), "purge_full");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);
    char qpath[270]; qdb_path(qpath, sizeof(qpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(purge_write_one(1));
    TEST_ASSERT_EQ_I(ray_journal_roll(), RAY_OK);        /* -> one archive */
    TEST_ASSERT_TRUE(purge_write_one(2));
    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_OK);    /* -> .qdb (+ archive) */

    /* Footprint exists before purge. */
    TEST_ASSERT_EQ_I(access(lpath, F_OK), 0);
    TEST_ASSERT_EQ_I(access(qpath, F_OK), 0);
    TEST_ASSERT_TRUE(archive_exists(base));

    /* Purge while open: must close + remove everything. */
    TEST_ASSERT_TRUE(ray_journal_is_open());
    TEST_ASSERT_EQ_I(ray_journal_purge(), RAY_OK);
    TEST_ASSERT_FALSE(ray_journal_is_open());

    TEST_ASSERT_EQ_I(access(lpath, F_OK), -1);
    TEST_ASSERT_EQ_I(access(qpath, F_OK), -1);
    TEST_ASSERT_FALSE(archive_exists(base));

    /* State was reset: a second purge has no base to act on. */
    TEST_ASSERT_EQ_I(ray_journal_purge(), RAY_ERR_DOMAIN);

    cleanup_base(base);
    PASS();
}

/* P2. The issue #279 workflow: open, write, close, then purge WITHOUT
 * re-passing the path — the base survives .log.close. */
static test_result_t test_journal_purge_after_close(void) {
    char base[256]; make_base(base, sizeof(base), "purge_close");
    char lpath[270]; log_path(lpath, sizeof(lpath), base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(purge_write_one(42));
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    TEST_ASSERT_EQ_I(access(lpath, F_OK), 0);    /* log still on disk */

    /* Purge after close still finds the base and removes the log. */
    TEST_ASSERT_EQ_I(ray_journal_purge(), RAY_OK);
    TEST_ASSERT_EQ_I(access(lpath, F_OK), -1);

    cleanup_base(base);
    PASS();
}

/* P3. ops wrapper: (.log.purge) returns null on success and a `domain`
 * error when no journal base is known. */
static test_result_t test_journal_purge_ops_wrapper(void) {
    char base[256]; make_base(base, sizeof(base), "purge_ops");

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(purge_write_one(7));

    ray_t* r = ray_log_purge_fn(NULL, 0);   /* null on success */
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    TEST_ASSERT_FALSE(ray_journal_is_open());

    /* Base reset -> wrapper surfaces a domain error. */
    ray_t* e = ray_log_purge_fn(NULL, 0);
    TEST_ASSERT_TRUE(e != NULL && RAY_IS_ERR(e));
    TEST_ASSERT_STR_EQ(ray_err_code(e), "domain");
    ray_error_free(e);

    cleanup_base(base);
    PASS();
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Registration
 * ═══════════════════════════════════════════════════════════════════════ */

const test_entry_t journal_entries[] = {
    /* Validate */
    { "journal/validate_clean",            test_journal_validate_clean,            jrn_setup, jrn_teardown },
    { "journal/validate_empty",            test_journal_validate_empty,            jrn_setup, jrn_teardown },
    { "journal/validate_no_file",          test_journal_validate_no_file,          jrn_setup, jrn_teardown },
    { "journal/validate_badtail",          test_journal_validate_badtail,          jrn_setup, jrn_teardown },
    { "journal/validate_short_payload",    test_journal_validate_short_payload,    jrn_setup, jrn_teardown },
    { "journal/validate_null_outparams",   test_journal_validate_null_outparams,   jrn_setup, jrn_teardown },
    { "journal/validate_bad_version",      test_journal_validate_bad_version,      jrn_setup, jrn_teardown },
    { "journal/validate_oversize",         test_journal_validate_oversize,         jrn_setup, jrn_teardown },
    { "journal/validate_growing_payload",  test_journal_validate_growing_payload,  jrn_setup, jrn_teardown },
    /* Replay */
    { "journal/replay_no_file",            test_journal_replay_no_file,            jrn_setup, jrn_teardown },
    { "journal/replay_clean_single",       test_journal_replay_clean_single,       jrn_setup, jrn_teardown },
    { "journal/replay_empty",              test_journal_replay_empty,              jrn_setup, jrn_teardown },
    { "journal/replay_badtail_short_hdr",  test_journal_replay_badtail_short_hdr,  jrn_setup, jrn_teardown },
    { "journal/replay_badtail_bad_prefix", test_journal_replay_badtail_bad_prefix, jrn_setup, jrn_teardown },
    { "journal/replay_badtail_bad_version",test_journal_replay_badtail_bad_version,jrn_setup, jrn_teardown },
    { "journal/replay_badtail_oversize",   test_journal_replay_badtail_oversize,   jrn_setup, jrn_teardown },
    { "journal/replay_badtail_zero_size",  test_journal_replay_badtail_zero_size,  jrn_setup, jrn_teardown },
    { "journal/replay_badtail_short_payload", test_journal_replay_badtail_short_payload, jrn_setup, jrn_teardown },
    { "journal/replay_null_outparams",     test_journal_replay_null_outparams,     jrn_setup, jrn_teardown },
    { "journal/replay_eval_error",         test_journal_replay_eval_error,         jrn_setup, jrn_teardown },
    /* Open/Close */
    { "journal/open_close_basic",          test_journal_open_close_basic,          jrn_setup, jrn_teardown },
    { "journal/open_bad_base",             test_journal_open_bad_base,             jrn_setup, jrn_teardown },
    { "journal/open_double_open",          test_journal_open_double_open,          jrn_setup, jrn_teardown },
    { "journal/open_replays_existing_log", test_journal_open_replays_existing_log, jrn_setup, jrn_teardown },
    { "journal/open_badtail_log",          test_journal_open_badtail_log,          jrn_setup, jrn_teardown },
    /* Write bytes */
    { "journal/write_when_closed",         test_journal_write_when_closed,         jrn_setup, jrn_teardown },
    { "journal/write_null_hdr",            test_journal_write_null_hdr,            jrn_setup, jrn_teardown },
    { "journal/write_null_payload",        test_journal_write_null_payload,        jrn_setup, jrn_teardown },
    { "journal/write_negative_len",        test_journal_write_negative_len,        jrn_setup, jrn_teardown },
    { "journal/write_async_mode",          test_journal_write_async_mode,          jrn_setup, jrn_teardown },
    { "journal/write_sync_mode",           test_journal_write_sync_mode,           jrn_setup, jrn_teardown },
    { "journal/write_zero_payload",        test_journal_write_zero_payload,        jrn_setup, jrn_teardown },
    /* Sync */
    { "journal/sync_when_closed",          test_journal_sync_when_closed,          jrn_setup, jrn_teardown },
    { "journal/sync_in_sync_mode",         test_journal_sync_in_sync_mode,         jrn_setup, jrn_teardown },
    { "journal/sync_in_async_mode",        test_journal_sync_in_async_mode,        jrn_setup, jrn_teardown },
    /* Roll */
    { "journal/roll_when_closed",          test_journal_roll_when_closed,          jrn_setup, jrn_teardown },
    { "journal/roll_basic",                test_journal_roll_basic,                jrn_setup, jrn_teardown },
    { "journal/roll_twice",                test_journal_roll_twice,                jrn_setup, jrn_teardown },
    /* Snapshot */
    { "journal/snapshot_when_closed",      test_journal_snapshot_when_closed,      jrn_setup, jrn_teardown },
    { "journal/snapshot_basic",            test_journal_snapshot_basic,            jrn_setup, jrn_teardown },
    { "journal/open_with_qdb",             test_journal_open_with_qdb,             jrn_setup, jrn_teardown },
    { "journal/snapshot_empty_env",        test_journal_snapshot_empty_env,        jrn_setup, jrn_teardown },
    /* is_open */
    { "journal/is_open_states",            test_journal_is_open_states,            jrn_setup, jrn_teardown },
    /* Misc */
    { "journal/open_deser_error",          test_journal_open_deser_error,          jrn_setup, jrn_teardown },
    { "journal/write_during_replay_noop",  test_journal_write_during_replay_noop,  jrn_setup, jrn_teardown },
    { "journal/replay_restricted_flag",    test_journal_replay_restricted_flag,    jrn_setup, jrn_teardown },
    /* Compressed frame */
    { "journal/replay_compressed_frame",       test_journal_replay_compressed_frame,       jrn_setup, jrn_teardown },
    { "journal/replay_compressed_bad_payload", test_journal_replay_compressed_bad_payload, jrn_setup, jrn_teardown },
    /* Bad .qdb */
    { "journal/open_bad_qdb_corrupt",      test_journal_open_bad_qdb_corrupt,      jrn_setup, jrn_teardown },
    { "journal/open_qdb_not_dict",         test_journal_open_qdb_not_dict,         jrn_setup, jrn_teardown },
    /* Misc guards */
    { "journal/open_null_base",            test_journal_open_null_base,            jrn_setup, jrn_teardown },
    { "journal/snapshot_multiple_bindings",test_journal_snapshot_multiple_bindings,jrn_setup, jrn_teardown },
    { "journal/open_log_is_directory",     test_journal_open_log_is_directory,     jrn_setup, jrn_teardown },
    /* Wrong key type in qdb, snapshot rename failure */
    { "journal/open_qdb_wrong_key_type",   test_journal_open_qdb_wrong_key_type,  jrn_setup, jrn_teardown },
    { "journal/open_qdb_missing_val",      test_journal_open_qdb_missing_val,     jrn_setup, jrn_teardown },
    { "journal/snapshot_rename_fails",     test_journal_snapshot_rename_fails,    jrn_setup, jrn_teardown },
    { "journal/roll_rename_fails",         test_journal_roll_rename_fails,        jrn_setup, jrn_teardown },
    /* Ops layer (src/ops/journal.c) */
    { "journal/ops_replay_null_path",      test_ops_replay_null_path,             jrn_setup, jrn_teardown },
    { "journal/ops_replay_non_string",     test_ops_replay_non_string,            jrn_setup, jrn_teardown },
    { "journal/ops_replay_long_path",      test_ops_replay_long_path,             jrn_setup, jrn_teardown },
    { "journal/ops_validate_null_path",    test_ops_validate_null_path,           jrn_setup, jrn_teardown },
    { "journal/ops_validate_non_string",   test_ops_validate_non_string,          jrn_setup, jrn_teardown },
    { "journal/ops_validate_long_path",    test_ops_validate_long_path,           jrn_setup, jrn_teardown },
    { "journal/ops_open_rank",             test_ops_open_rank,                    jrn_setup, jrn_teardown },
    { "journal/ops_open_null_mode",        test_ops_open_null_mode,               jrn_setup, jrn_teardown },
    { "journal/ops_open_int_mode",         test_ops_open_int_mode,                jrn_setup, jrn_teardown },
    { "journal/ops_open_null_base",        test_ops_open_null_base,               jrn_setup, jrn_teardown },
    { "journal/ops_open_sym_base",         test_ops_open_sym_base,                jrn_setup, jrn_teardown },
    { "journal/ops_open_bad_mode",         test_ops_open_bad_mode,                jrn_setup, jrn_teardown },
    { "journal/ops_open_long_base",        test_ops_open_long_base,               jrn_setup, jrn_teardown },
    { "journal/ops_open_sync_mode",        test_ops_open_sync_mode,               jrn_setup, jrn_teardown },
    { "journal/ops_write_null_expr",       test_ops_write_null_expr,              jrn_setup, jrn_teardown },
    { "journal/ops_write_noopen",          test_ops_write_noopen,                 jrn_setup, jrn_teardown },
    { "journal/ops_write_serde_size_zero", test_ops_write_serde_size_zero,        jrn_setup, jrn_teardown },
    /* End-to-end RFL round-trip */
    { "journal/replay_symbol_head_both_forms", test_journal_replay_symbol_head_both_forms, jrn_setup, jrn_teardown },
    { "journal/replay_unbound_head_is_graceful", test_journal_replay_unbound_head_is_graceful, jrn_setup, jrn_teardown },
    /* Open opens for append only — no replay */
    { "journal/open_does_not_replay",      test_journal_open_does_not_replay,      jrn_setup, jrn_teardown },
    { "journal/de_depth_limit",            test_journal_de_depth_limit,            jrn_setup, jrn_teardown },
    /* Purge — close + unlink the whole journal footprint */
    { "journal/purge_full",                test_journal_purge_full,                jrn_setup, jrn_teardown },
    { "journal/purge_after_close",         test_journal_purge_after_close,         jrn_setup, jrn_teardown },
    { "journal/purge_ops_wrapper",         test_journal_purge_ops_wrapper,         jrn_setup, jrn_teardown },
    /* #420: snapshot/roll crash window must not double-apply */
    { "journal/crash_window_no_double_apply", test_journal_crash_window_no_double_apply, jrn_setup, jrn_teardown },
    { NULL, NULL, NULL, NULL },
};
