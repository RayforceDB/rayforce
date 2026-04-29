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

#define _DEFAULT_SOURCE   /* mkdtemp, strdup */

#include "test.h"
#include <rayforce.h>
#include <rayforce.h>
#include "core/runtime.h"   /* ray_runtime_t, ray_runtime_create*, __RUNTIME */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

static char* make_tmpdir(void) {
    char tmpl[] = "/tmp/rayforce-rt-test-XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) return NULL;
    return strdup(tmpl);
}

/* Absent sym file: stat fails with ENOENT, which is the "first run"
 * normal case.  out_sym_err must stay RAY_OK and runtime must come up. */
static test_result_t test_create_with_sym_absent_is_ok(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);
    char path[256];
    snprintf(path, sizeof(path), "%s/missing.sym", dir);

    ray_err_t err = RAY_ERR_OOM;  /* poison — should be overwritten */
    ray_runtime_t* rt = ray_runtime_create_with_sym_err(path, &err);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQ_I((int)err, (int)RAY_OK);

    ray_runtime_destroy(rt);
    rmdir(dir);
    free(dir);
    PASS();
}

/* Non-ENOENT stat failure must surface as RAY_ERR_IO.  We hit this by
 * passing a path whose parent exists but isn't a directory (ENOTDIR) —
 * portable across Linux/macOS without needing root or chmod games. */
static test_result_t test_create_with_sym_io_error_surfaces(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);

    /* Create a regular file, then ask to stat a path that treats it as a
     * directory prefix — POSIX returns ENOTDIR. */
    char blocker[256], path[256];
    snprintf(blocker, sizeof(blocker), "%s/not-a-dir", dir);
    snprintf(path, sizeof(path), "%s/not-a-dir/sym", dir);
    FILE* f = fopen(blocker, "w");
    TEST_ASSERT_NOT_NULL(f);
    fclose(f);

    ray_err_t err = RAY_OK;
    ray_runtime_t* rt = ray_runtime_create_with_sym_err(path, &err);
    TEST_ASSERT_NOT_NULL(rt);
    /* Pin the exact error code — the contract maps every non-ENOENT
     * stat failure to RAY_ERR_IO, so drift in the mapping should fail
     * this test loudly. */
    TEST_ASSERT_EQ_I((int)err, (int)RAY_ERR_IO);

    ray_runtime_destroy(rt);
    unlink(blocker);
    rmdir(dir);
    free(dir);
    PASS();
}

/* The plain (non-_err) variant discards load result; runtime still comes
 * up cleanly regardless of sym-file state. */
static test_result_t test_create_with_sym_plain_variant_absent(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);
    char path[256];
    snprintf(path, sizeof(path), "%s/also-missing.sym", dir);

    ray_runtime_t* rt = ray_runtime_create_with_sym(path);
    TEST_ASSERT_NOT_NULL(rt);

    ray_runtime_destroy(rt);
    rmdir(dir);
    free(dir);
    PASS();
}

/* Corrupt sym file must surface as RAY_ERR_CORRUPT via the _err variant
 * (not silently downgraded to RAY_OK).  We fake a corrupt file by
 * writing random bytes — ray_sym_load expects a serialized RAY_LIST of
 * -RAY_STR entries, so arbitrary bytes will fail its header validation. */
static test_result_t test_create_with_sym_corrupt_file(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);

    char path[256];
    snprintf(path, sizeof(path), "%s/corrupt.sym", dir);
    FILE* f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL(f);
    /* Write STR_LIST_MAGIC ("STRL" little-endian) followed by a truncated
     * payload — header-count byte count=999 but no body — ray_col_load
     * will hit col_load_str_list's "corrupt" path, which maps to
     * RAY_ERR_CORRUPT via ray_err_from_obj. */
    uint32_t magic = 0x4C525453U;  /* STR_LIST_MAGIC */
    int64_t count = 999;           /* claims 999 strings, none present */
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&count, sizeof(count), 1, f);
    fclose(f);

    ray_err_t err = RAY_OK;
    ray_runtime_t* rt = ray_runtime_create_with_sym_err(path, &err);
    TEST_ASSERT_NOT_NULL(rt);
    /* Pin the exact error code: the contract maps corrupted sym data
     * to RAY_ERR_CORRUPT, distinct from I/O or OOM, so callers can
     * decide recovery policy. */
    TEST_ASSERT_EQ_I((int)err, (int)RAY_ERR_CORRUPT);

    ray_runtime_destroy(rt);
    unlink(path);
    rmdir(dir);
    free(dir);
    PASS();
}

/* Load-before-builtins ordering is the whole reason
 * ray_runtime_create_with_sym exists: after a save/destroy/load cycle,
 * user-interned sym IDs must occupy exactly the slots they had before,
 * while builtins append afterwards.  Intern a distinctive name, save,
 * tear down, reload via the persistent-consumer entrypoint, and verify
 * the same string interns to the same ID. */
static test_result_t test_create_with_sym_load_preserves_user_ids(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);

    char path[256];
    snprintf(path, sizeof(path), "%s/ids.sym", dir);

    /* Phase 1: intern a name then persist the sym table. */
    ray_runtime_t* rt1 = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt1);
    int64_t id_before = ray_sym_intern("rayforce-user-marker", 20);
    TEST_ASSERT_EQ_I((int)ray_sym_save(path), (int)RAY_OK);
    ray_runtime_destroy(rt1);

    /* Phase 2: bring up a fresh runtime via the _with_sym variant so the
     * persisted table is loaded before builtins register. */
    ray_err_t err = RAY_ERR_OOM;
    ray_runtime_t* rt2 = ray_runtime_create_with_sym_err(path, &err);
    TEST_ASSERT_NOT_NULL(rt2);
    TEST_ASSERT_EQ_I((int)err, (int)RAY_OK);

    /* Same string must re-intern to the same ID (not shift because of
     * builtins claiming the low slots first). */
    int64_t id_after = ray_sym_intern("rayforce-user-marker", 20);
    TEST_ASSERT_EQ_I((int)id_after, (int)id_before);

    ray_runtime_destroy(rt2);
    unlink(path);
    /* ray_sym_save may also create a lock file. */
    char lock_path[320];
    snprintf(lock_path, sizeof(lock_path), "%s.lk", path);
    unlink(lock_path);
    rmdir(dir);
    free(dir);
    PASS();
}

/* Sym file whose stat st_size exceeds mem_budget/2 must trigger the
 * pre-flight OOM guard and surface RAY_ERR_OOM through out_sym_err.
 * We use ftruncate to create a sparse file without actually allocating
 * the backing bytes.  Budget auto-detects ~80% of RAM, so a sparse
 * file ~10 EB guarantees tripping the half-budget ceiling on any
 * realistic dev/CI host. */
static test_result_t test_create_with_sym_oversized_file(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);

    /* Skip on platforms with 32-bit off_t — the sparse size we want
     * (>> 4 GB) isn't representable and the shift in that case would
     * be undefined. */
    if (sizeof(off_t) < 8) {
        free(dir);
        SKIP("explicit MUNIT_SKIP");
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/huge.sym", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT((fd) >= (0), "fd >= 0");
    /* 4 EB sparse — bigger than any plausible mem_budget/2 (<1 ZB of
     * RAM).  Build via int64_t to keep the shift well-defined, then
     * cast to off_t after the width guard above has passed. */
    int64_t huge64 = (int64_t)1 << 62;
    off_t huge = (off_t)huge64;
    int rc = ftruncate(fd, huge);
    close(fd);
    if (rc != 0) {
        /* Some filesystems (tmpfs on limited hosts) reject the giant
         * ftruncate — skip rather than fail spuriously. */
        unlink(path);
        rmdir(dir);
        free(dir);
        SKIP("explicit MUNIT_SKIP");
    }

    ray_err_t err = RAY_OK;
    ray_runtime_t* rt = ray_runtime_create_with_sym_err(path, &err);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQ_I((int)err, (int)RAY_ERR_OOM);

    ray_runtime_destroy(rt);
    unlink(path);
    rmdir(dir);
    free(dir);
    PASS();
}

/* ---- OOM sentinel correctness ---------------------------------------
 *
 * RAY_OOM_OBJ is the static fallback ray_error returns when its own
 * ray_alloc fails (deep OOM).  It must satisfy three contracts:
 *   1. RAY_IS_ERR(RAY_OOM_OBJ) == true        — upstream guards work
 *   2. ray_err_code(RAY_OOM_OBJ) == "oom"     — diagnostics work
 *   3. ray_error_free(RAY_OOM_OBJ) is a no-op — won't corrupt the heap
 * Without all three, deep OOM regresses to silent NULL or a free-of-BSS
 * crash. */
static test_result_t test_oom_sentinel_is_well_formed(void) {
    TEST_ASSERT_NOT_NULL(RAY_OOM_OBJ);
    TEST_ASSERT_TRUE(RAY_IS_ERR(RAY_OOM_OBJ));
    const char* code = ray_err_code(RAY_OOM_OBJ);
    TEST_ASSERT_NOT_NULL(code);
    TEST_ASSERT_EQ_I(strcmp(code, "oom"), 0);

    /* Must not corrupt anything when "freed" — the sentinel lives in BSS
     * and is shared by every deep-OOM caller; any one freeing it would
     * unlink the shared object. */
    ray_error_free(RAY_OOM_OBJ);
    /* Re-check: still well-formed after a "free" attempt. */
    TEST_ASSERT_TRUE(RAY_IS_ERR(RAY_OOM_OBJ));
    TEST_ASSERT_EQ_I(strcmp(ray_err_code(RAY_OOM_OBJ), "oom"), 0);
    PASS();
}

const test_entry_t runtime_entries[] = {
    { "runtime/create_with_sym_absent_is_ok", test_create_with_sym_absent_is_ok, NULL, NULL },
    { "runtime/create_with_sym_io_error_surfaces", test_create_with_sym_io_error_surfaces, NULL, NULL },
    { "runtime/create_with_sym_plain_variant_absent", test_create_with_sym_plain_variant_absent, NULL, NULL },
    { "runtime/create_with_sym_corrupt_file", test_create_with_sym_corrupt_file, NULL, NULL },
    { "runtime/create_with_sym_load_preserves_user_ids", test_create_with_sym_load_preserves_user_ids, NULL, NULL },
    { "runtime/create_with_sym_oversized_file", test_create_with_sym_oversized_file, NULL, NULL },
    { "runtime/oom_sentinel_is_well_formed", test_oom_sentinel_is_well_formed, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};


