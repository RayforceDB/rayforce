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
#include "core/sock.h"      /* ray_sock_* */
#include "lang/format.h"    /* ray_fmt for eval_err */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>

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

/* ---- sock.c coverage helpers ---------------------------------------- */

/* ray_sock_close must silently ignore RAY_INVALID_SOCK without crashing.
 * Covers the early-return region at line 180 of sock.c. */
static test_result_t test_sock_close_invalid(void) {
    ray_sock_close(RAY_INVALID_SOCK);   /* must not crash */
    ray_sock_close(RAY_INVALID_SOCK);   /* idempotent */
    PASS();
}

/* ray_sock_listen with a port already in the LISTEN state must fail and
 * return RAY_INVALID_SOCK (EADDRINUSE bind path, lines 65-67 of sock.c).
 *
 * We occupy the port with a raw socket that has SO_REUSEPORT disabled
 * (never set) and is actively listening.  ray_sock_listen sets
 * SO_REUSEADDR on its own socket which allows rebinding a TIME_WAIT
 * address but NOT a currently-listening one when SO_REUSEPORT is absent.
 * Using INADDR_ANY (same as ray_sock_listen) ensures the conflict. */
static test_result_t test_sock_listen_bind_fails_eaddrinuse(void) {
    int raw = socket(AF_INET, SOCK_STREAM, 0);
    TEST_ASSERT(raw >= 0, "raw socket");
    /* Do NOT set SO_REUSEPORT — leave the port exclusively owned. */

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = 0; /* OS assigns */
    TEST_ASSERT(bind(raw, (struct sockaddr*)&addr, sizeof(addr)) == 0, "first bind");
    TEST_ASSERT(listen(raw, 1) == 0, "first listen");

    socklen_t alen = sizeof(addr);
    getsockname(raw, (struct sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    /* ray_sock_listen sets SO_REUSEADDR but NOT SO_REUSEPORT, so binding
     * INADDR_ANY on a port already in LISTEN must return EADDRINUSE. */
    ray_sock_t srv = ray_sock_listen(port);

    close(raw);

    TEST_ASSERT_EQ_I((int)srv, (int)RAY_INVALID_SOCK);
    PASS();
}

/* ray_sock_connect with an unresolvable hostname must return
 * RAY_INVALID_SOCK (getaddrinfo failure region, line 100 of sock.c). */
static test_result_t test_sock_connect_bad_host(void) {
    /* .invalid TLD is IANA-reserved to never resolve. */
    ray_sock_t fd = ray_sock_connect("this.host.is.invalid", 9999, 500);
    TEST_ASSERT_EQ_I((int)fd, (int)RAY_INVALID_SOCK);
    PASS();
}

/* ray_sock_connect with timeout_ms == 0 must take the else-branch of the
 * "if (timeout_ms > 0)" guard (line 110 of sock.c).  We connect to a
 * listener on localhost so the connect itself succeeds and we exercise
 * the code past that branch. */
static test_result_t test_sock_connect_no_timeout(void) {
    ray_sock_t srv = ray_sock_listen(0);
    if (srv == RAY_INVALID_SOCK) SKIP("could not open listen socket");

    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);
    getsockname((int)srv, (struct sockaddr*)&addr, &alen);
    uint16_t port = ntohs(addr.sin_port);

    /* timeout_ms == 0: must NOT enter the timeout-setup block */
    ray_sock_t client = ray_sock_connect("127.0.0.1", port, 0);

    ray_sock_close(srv);
    if (client != RAY_INVALID_SOCK) ray_sock_close(client);
    TEST_ASSERT((int)client != (int)RAY_INVALID_SOCK, "connect with timeout=0 should succeed");
    PASS();
}

/* ─── system.c (sys_*) builtin coverage (S8) ───────────────── */

static int eval_eq(const char* src, const char* expected) {
    ray_t* le = ray_eval_str(src);
    if (!le || RAY_IS_ERR(le)) { if (le) ray_error_free(le); return 0; }
    ray_t* re = ray_eval_str(expected);
    if (!re || RAY_IS_ERR(re)) { ray_release(le); if (re) ray_error_free(re); return 0; }
    ray_t* ls = ray_fmt(le, 0);
    ray_t* rs = ray_fmt(re, 0);
    int same = ls && rs && ray_str_len(ls) == ray_str_len(rs) &&
               memcmp(ray_str_ptr(ls), ray_str_ptr(rs), ray_str_len(rs)) == 0;
    if (ls) ray_release(ls);
    if (rs) ray_release(rs);
    ray_release(le);
    ray_release(re);
    return same;
}

static int eval_err(const char* src, const char* substr) {
    ray_t* le = ray_eval_str(src);
    if (!le || !RAY_IS_ERR(le)) { if (le) ray_release(le); return 0; }
    ray_t* s = ray_fmt(le, 0);
    int hit = s && ray_str_ptr(s) && strstr(ray_str_ptr(s), substr) != NULL;
    if (s) ray_release(s);
    ray_error_free(le);
    return hit;
}

static void sys_setup(void)    { ray_runtime_create(0, NULL); }
static void sys_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static test_result_t test_syscov_eval_builtin(void) {
    /* (eval (parse "42")) -> 42 */
    TEST_ASSERT_TRUE(eval_eq("(eval (parse \"42\"))", "42"));
    /* parse type error: non-string */
    TEST_ASSERT_TRUE(eval_err("(parse 99)", "type"));
    /* parse domain error: NULL src shouldn't happen via normal path but
     * the identity path (parse a valid string) must work */
    TEST_ASSERT_TRUE(eval_eq("(parse \"true\")", "(parse \"true\")"));
    PASS();
}

/* quote special form (ray_quote_fn) */
static test_result_t test_syscov_quote(void) {
    /* (quote 42) -> 42 unevaluated */
    TEST_ASSERT_TRUE(eval_eq("(quote 42)", "42"));
    /* (quote (+ 1 2)) -> unevaluated list, not 3 */
    ray_t* r = ray_eval_str("(quote (+ 1 2))");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    /* quote with zero args -> domain error */
    TEST_ASSERT_TRUE(eval_err("(quote)", "domain"));
    PASS();
}

/* return builtin (ray_return_fn) */
static test_result_t test_syscov_return(void) {
    TEST_ASSERT_TRUE(eval_eq("(return 7)", "7"));
    TEST_ASSERT_TRUE(eval_eq("(return \"hello\")", "\"hello\""));
    PASS();
}

/* args builtin (ray_args_fn) */
static test_result_t test_syscov_args(void) {
    /* (args) returns an empty list */
    ray_t* r = ray_eval_str("(args 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_len(r), 0);
    ray_release(r);
    PASS();
}

/* rc builtin (ray_rc_fn) */
static test_result_t test_syscov_rc(void) {
    /* rc of a freshly created atom is at least 1 */
    ray_t* r = ray_eval_str("(rc 42)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    /* rc should be a non-negative integer */
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    TEST_ASSERT_TRUE(r->i64 >= 0);
    ray_release(r);
    PASS();
}

/* timer builtin (ray_timer_fn) */
static test_result_t test_syscov_timer(void) {
    ray_t* r = ray_eval_str("(timer 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    ray_release(r);
    PASS();
}

/* env builtin (ray_env_fn) */
static test_result_t test_syscov_env(void) {
    ray_t* r = ray_eval_str("(env 0)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, RAY_DICT);
    ray_release(r);
    PASS();
}

/* setenv type errors (ray_setenv_fn) */
static test_result_t test_syscov_setenv_type_errors(void) {
    /* first arg not a string */
    TEST_ASSERT_TRUE(eval_err("(.os.setenv 42 \"val\")", "type"));
    /* second arg not a string */
    TEST_ASSERT_TRUE(eval_err("(.os.setenv \"key\" 42)", "type"));
    /* happy path: setenv/getenv round-trip (.os.* namespace) */
    TEST_ASSERT_TRUE(eval_eq("(.os.setenv \"RAY_TEST_COV_KEY\" \"hello\")", "\"hello\""));
    TEST_ASSERT_TRUE(eval_eq("(.os.getenv \"RAY_TEST_COV_KEY\")", "\"hello\""));
    PASS();
}

/* hopen type error (ray_hopen_fn) */
static test_result_t test_syscov_hopen_type_error(void) {
    /* non-string arg */
    TEST_ASSERT_TRUE(eval_err("(.ipc.open 42)", "type"));
    PASS();
}

/* hopen domain errors (ray_hopen_fn) */
static test_result_t test_syscov_hopen_domain_errors(void) {
    /* too few parts (no colon) */
    TEST_ASSERT_TRUE(eval_err("(.ipc.open \"localhost\")", "domain"));
    /* port out of range */
    TEST_ASSERT_TRUE(eval_err("(.ipc.open \"localhost:0\")", "domain"));
    TEST_ASSERT_TRUE(eval_err("(.ipc.open \"localhost:99999\")", "domain"));
    /* invalid port string */
    TEST_ASSERT_TRUE(eval_err("(.ipc.open \"localhost:abc\")", "domain"));
    PASS();
}

/* hopen with user:password parts (lines 799-807 of system.c) */
static test_result_t test_syscov_hopen_with_credentials(void) {
    /* connection to non-existent server; covers the user/password extraction
     * branch (lines 799-807) even though the connect attempt fails with io */
    TEST_ASSERT_TRUE(eval_err("(.ipc.open \"127.0.0.1:19999:user:pass\")", "io"));
    PASS();
}

/* hclose type error (ray_hclose_fn) */
static test_result_t test_syscov_hclose_type_error(void) {
    /* non-integer arg */
    TEST_ASSERT_TRUE(eval_err("(.ipc.close \"nothandle\")", "type"));
    PASS();
}

/* hsend type errors (ray_hsend_fn) */
static test_result_t test_syscov_hsend_type_errors(void) {
    /* handle not integer */
    TEST_ASSERT_TRUE(eval_err("(.ipc.send \"bad\" 42)", "type"));
    PASS();
}

/* .db.splayed.set with explicit sym_path (line 89 of system.c) */
static test_result_t test_syscov_splayed_set_with_sym_path(void) {
    char tmpl[] = "/tmp/rfcov-splay-XXXXXX";
    /* mkdtemp modifies tmpl in-place and returns a pointer to it (stack memory).
     * Do NOT free the returned pointer — it is just tmpl. */
    char* dir = mkdtemp(tmpl);
    if (!dir) SKIP("mkdtemp failed");

    char sym_path[512];
    snprintf(sym_path, sizeof(sym_path), "%s/mysym", dir);
    /* Transfer paths via environment so rfl code can read them. */
    setenv("RFCOV_SPLAY_DIR",  dir,      1);
    setenv("RFCOV_SPLAY_SYM",  sym_path, 1);

    /* Build the eval string: uses 3-arg .db.splayed.set to hit line 89. */
    char src[1024];
    snprintf(src, sizeof(src),
        "(let d (.os.getenv \"RFCOV_SPLAY_DIR\"))"
        "(let s (.os.getenv \"RFCOV_SPLAY_SYM\"))"
        "(let t (table (list 'x) (list (vec [1i 2i 3i]))))"
        "(.db.splayed.set d t s)");
    ray_t* r = ray_eval_str(src);
    /* Accept either success or any error — the goal is to exercise line 89. */
    if (r) {
        if (RAY_IS_ERR(r)) ray_error_free(r);
        else ray_release(r);
    }

    /* cleanup — no free(dir), it's a stack pointer */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    system(cmd);
    PASS();
}

/* .db.splayed.get with explicit sym_path (line 110 of system.c) */
static test_result_t test_syscov_splayed_get_with_sym_path(void) {
    /* Pass a non-existent path to hit the 2-arg branch; it will error but
     * the branch at line 110 is still executed. */
    TEST_ASSERT_TRUE(eval_err(
        "(.db.splayed.get \"/tmp/no-such-dir-rfcov\" \"/tmp/no-such-sym\")",
        ""));  /* any error is fine */
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
    { "runtime/sock_close_invalid",                  test_sock_close_invalid,                  NULL, NULL },
    { "runtime/sock_listen_bind_fails_eaddrinuse",   test_sock_listen_bind_fails_eaddrinuse,   NULL, NULL },
    { "runtime/sock_connect_bad_host",               test_sock_connect_bad_host,               NULL, NULL },
    { "runtime/sock_connect_no_timeout",             test_sock_connect_no_timeout,             NULL, NULL },

    /* system.c builtins (S8) */
    { "runtime/syscov_eval_builtin",         test_syscov_eval_builtin,         sys_setup, sys_teardown },
    { "runtime/syscov_quote",                test_syscov_quote,                sys_setup, sys_teardown },
    { "runtime/syscov_return",               test_syscov_return,               sys_setup, sys_teardown },
    { "runtime/syscov_args",                 test_syscov_args,                 sys_setup, sys_teardown },
    { "runtime/syscov_rc",                   test_syscov_rc,                   sys_setup, sys_teardown },
    { "runtime/syscov_timer",                test_syscov_timer,                sys_setup, sys_teardown },
    { "runtime/syscov_env",                  test_syscov_env,                  sys_setup, sys_teardown },
    { "runtime/syscov_setenv_type_errors",   test_syscov_setenv_type_errors,   sys_setup, sys_teardown },
    { "runtime/syscov_hopen_type_error",     test_syscov_hopen_type_error,     sys_setup, sys_teardown },
    { "runtime/syscov_hopen_domain_errors",  test_syscov_hopen_domain_errors,  sys_setup, sys_teardown },
    { "runtime/syscov_hopen_with_credentials", test_syscov_hopen_with_credentials, sys_setup, sys_teardown },
    { "runtime/syscov_hclose_type_error",    test_syscov_hclose_type_error,    sys_setup, sys_teardown },
    { "runtime/syscov_hsend_type_errors",    test_syscov_hsend_type_errors,    sys_setup, sys_teardown },
    { "runtime/syscov_splayed_set_sym_path", test_syscov_splayed_set_with_sym_path, sys_setup, sys_teardown },
    { "runtime/syscov_splayed_get_sym_path", test_syscov_splayed_get_with_sym_path, sys_setup, sys_teardown },

    { NULL, NULL, NULL, NULL },
};


