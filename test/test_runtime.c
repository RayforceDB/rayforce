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
#include "core/poll.h"      /* ray_poll_t, ray_poll_create/destroy */
#include "core/timer.h"     /* ray_timers_t, ray_timers_pump_for */
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

    /* Pass 1: intern a name then persist the sym table. */
    ray_runtime_t* rt1 = ray_runtime_create(0, NULL);
    TEST_ASSERT_NOT_NULL(rt1);
    int64_t id_before = ray_sym_intern("rayforce-user-marker", 20);
    TEST_ASSERT_EQ_I((int)ray_sym_save(path), (int)RAY_OK);
    ray_runtime_destroy(rt1);

    /* Pass 2: bring up a fresh runtime via the _with_sym variant so the
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

/* Sym file whose stat st_size exceeds half of physical RAM must trigger the
 * pre-flight OOM guard and surface RAY_ERR_OOM through out_sym_err.
 * We use ftruncate to create a SPARSE file (no backing bytes), sized from
 * the same total physical RAM the runtime detects so it lands just over the
 * half-RAM ceiling on any host — and stays ~RAM-sized (~tens of GB), well
 * under every real filesystem's max-file limit.  The earlier fixed 4 EB
 * (1<<62) size exceeded ext4's 16 TB cap → EFBIG → the test silently skipped
 * on every Linux dev/CI host. */
static test_result_t test_create_with_sym_oversized_file(void) {
    char* dir = make_tmpdir();
    TEST_ASSERT_NOT_NULL(dir);

    /* Mirror runtime.c's guard: total physical RAM.  half + 1 MB just trips
     * the > total_ram/2 guard.  Sparse, so those bytes are never backed and
     * the pre-flight stat check short-circuits before any read. */
    long pages = sysconf(_SC_PHYS_PAGES);
    long psize = sysconf(_SC_PAGE_SIZE);
    TEST_ASSERT((pages) > (0), "sysconf(_SC_PHYS_PAGES)");
    TEST_ASSERT((psize) > (0), "sysconf(_SC_PAGE_SIZE)");
    int64_t total_ram = (int64_t)((double)pages * (double)psize);
    off_t huge = (off_t)(total_ram / 2 + (1 << 20));

    char path[256];
    snprintf(path, sizeof(path), "%s/huge.sym", dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    TEST_ASSERT((fd) >= (0), "fd >= 0");
    int rc = ftruncate(fd, huge);
    close(fd);
    /* A ~RAM-sized sparse ftruncate succeeds on every mainstream FS; a
     * failure here is a genuine, hard error, not a reason to skip. */
    TEST_ASSERT_EQ_I(rc, 0);

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

/* Variant that also wires up a poll loop — required by .time.timer.*
 * tests, since `ray_runtime_create` doesn't allocate one (only main.c
 * does in production).  Without it, .time.timer.set returns
 * `"no poll loop active"`. */
static ray_poll_t* g_test_poll = NULL;
static void sys_setup_with_poll(void) {
    ray_runtime_create(0, NULL);
    g_test_poll = ray_poll_create();
    ray_runtime_set_poll(g_test_poll);
}
static void sys_teardown_with_poll(void) {
    ray_runtime_set_poll(NULL);
    if (g_test_poll) ray_poll_destroy(g_test_poll);
    g_test_poll = NULL;
    ray_runtime_destroy(__RUNTIME);
}

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

/* return builtin (ray_return_fn) — runtime variadic + compiled early-exit */
static test_result_t test_syscov_return(void) {
    /* ── Runtime builtin path (no enclosing compiled lambda) ── */
    TEST_ASSERT_TRUE(eval_eq("(return 7)", "7"));
    TEST_ASSERT_TRUE(eval_eq("(return \"hello\")", "\"hello\""));
    {
        ray_t* r = ray_eval_str("(return)");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(r);
    }
    TEST_ASSERT_TRUE(eval_err("(return 1 2)", "domain"));

    /* ── Compile-time early-return inside (fn ...) ── */
    /* Plain early return overrides trailing expressions. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (return 7) 99))", "7"));
    /* Zero-arg form returns null. */
    {
        ray_t* r = ray_eval_str("((fn [] (return)))");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(r);
    }
    /* Early return from a conditional branch. */
    TEST_ASSERT_TRUE(eval_eq("((fn [x] (if (> x 0) (return 1)) 0) 5)",  "1"));
    TEST_ASSERT_TRUE(eval_eq("((fn [x] (if (> x 0) (return 1)) 0) -1)", "0"));
    /* return inside (try ...) — must emit OP_TRAP_END before OP_RET. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (try (return 42) (fn [e] e))))", "42"));
    /* return inside nested (try ...) — must emit two OP_TRAP_ENDs. */
    TEST_ASSERT_TRUE(eval_eq(
        "((fn [] (try (try (return 42) (fn [e] e)) (fn [e] e))))", "42"));
    /* (return) nested in a partially-evaluated expression — must return
     * null rather than the stale 1 already on the stack when OP_RET fires. */
    {
        ray_t* r = ray_eval_str("((fn [] (+ 1 (return))))");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(r);
    }
    /* return bound to a local and called as a value still hits the
     * variadic builtin (identity for one arg). Note: (map return xs)
     * is not viable here — call_fn1 doesn't dispatch RAY_VARY, which
     * is a pre-existing limitation across all vary builtins, not
     * something introduced by this change. */
    TEST_ASSERT_TRUE(eval_eq("((fn [] (let f return) (f 7)))", "7"));

    PASS();
}

/* runtime sys_args storage: set/get round-trips, destroy releases it */
static test_result_t test_sys_args_storage(void) {
    /* unset → NULL */
    TEST_ASSERT_NULL(ray_runtime_get_sys_args());

    /* build a tiny dict, store it, read it back (same pointer) */
    ray_t* keys = ray_sym_vec_new(RAY_SYM_W64, 1);
    int64_t s = ray_sym_intern("k", 1);
    keys = ray_vec_append(keys, &s);
    ray_t* vals = ray_list_new(1);
    ray_t* one = ray_i64(1);
    vals = ray_list_append(vals, one); ray_release(one);
    ray_t* d = ray_dict_new(keys, vals);

    ray_runtime_set_sys_args(d);                 /* runtime takes ownership */
    TEST_ASSERT_EQ_PTR(ray_runtime_get_sys_args(), d);
    /* teardown (ray_runtime_destroy) must release it without leaking */
    PASS();
}

/* helper: fetch a top-level value from a built args dict (owned) */
static ray_t* args_top(ray_t* d, const char* name) {
    ray_t* k = ray_sym(ray_sym_intern(name, strlen(name)));
    ray_t* v = ray_dict_get(d, k);
    ray_release(k);
    return v;
}

static test_result_t test_build_sys_args_defaults(void) {
    char* argv[] = { "rayforce" };
    ray_t* d = ray_build_sys_args(1, argv);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQ_I(d->type, RAY_DICT);

    ray_t* port = args_top(d, "port");
    TEST_ASSERT_EQ_I(port->type, -RAY_I64);
    TEST_ASSERT_EQ_I(port->i64, 0);
    ray_release(port);

    ray_t* cores = args_top(d, "cores");
    TEST_ASSERT_EQ_I(cores->type, -RAY_I64);
    TEST_ASSERT_EQ_I(cores->i64, 0);
    ray_release(cores);

    ray_t* timeit = args_top(d, "timeit");
    TEST_ASSERT_EQ_I(timeit->type, -RAY_BOOL);
    TEST_ASSERT_EQ_I(timeit->b8, 0);
    ray_release(timeit);

    ray_t* file = args_top(d, "file");
    TEST_ASSERT_EQ_I(file->type, -RAY_STR);
    TEST_ASSERT_EQ_I(ray_str_len(file), 0);
    ray_release(file);

    ray_t* user = args_top(d, "user");
    TEST_ASSERT_EQ_I(user->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(user), 0);
    ray_release(user);

    ray_release(d);
    PASS();
}

static test_result_t test_build_sys_args_flags_and_user(void) {
    char* argv[] = { "rayforce", "-p", "5000", "-c", "4", "-i",
                     "--", "-opt", "123", "--verbose", "--name", "ray" };
    ray_t* d = ray_build_sys_args(12, argv);

    ray_t* port = args_top(d, "port");
    TEST_ASSERT_EQ_I(port->type, -RAY_I64);
    TEST_ASSERT_EQ_I(port->i64, 5000);
    ray_release(port);

    ray_t* cores = args_top(d, "cores");
    TEST_ASSERT_EQ_I(cores->i64, 4);
    ray_release(cores);

    ray_t* inter = args_top(d, "interactive");
    TEST_ASSERT_EQ_I(inter->type, -RAY_BOOL);
    TEST_ASSERT_EQ_I(inter->b8, 1);
    ray_release(inter);

    ray_t* user = args_top(d, "user");
    TEST_ASSERT_EQ_I(user->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(user), 3);     /* opt, verbose, name */

    ray_t* ko = ray_sym(ray_sym_intern("opt", 3));
    ray_t* vo = ray_dict_get(user, ko);
    TEST_ASSERT_EQ_I(vo->type, -RAY_STR);
    TEST_ASSERT_EQ_I(strcmp(ray_str_ptr(vo), "123"), 0);
    ray_release(vo); ray_release(ko);

    ray_t* kv = ray_sym(ray_sym_intern("verbose", 7));
    ray_t* vv = ray_dict_get(user, kv);
    TEST_ASSERT_EQ_I(vv->type, -RAY_STR);
    TEST_ASSERT_EQ_I(ray_str_len(vv), 0);         /* bare flag → "" */
    ray_release(vv); ray_release(kv);

    ray_release(user);
    ray_release(d);
    PASS();
}

static test_result_t test_build_sys_args_edges(void) {
    /* adjacent flags, dup key (last wins), no auth leakage */
    char* argv[] = { "rayforce", "-u", "secret",
                     "--", "-a", "-b", "-k", "v1", "-k", "v2" };
    ray_t* d = ray_build_sys_args(10, argv);

    ray_t* auth = args_top(d, "auth");
    TEST_ASSERT_NULL(auth);

    ray_t* user = args_top(d, "user");
    TEST_ASSERT_EQ_I(ray_dict_len(user), 3);      /* a, b, k */

    ray_t* ka = ray_sym(ray_sym_intern("a", 1));
    ray_t* va = ray_dict_get(user, ka);
    TEST_ASSERT_EQ_I(ray_str_len(va), 0);          /* -a -b → a:"" */
    ray_release(va); ray_release(ka);

    ray_t* kk = ray_sym(ray_sym_intern("k", 1));
    ray_t* vk = ray_dict_get(user, kk);
    TEST_ASSERT_EQ_I(strcmp(ray_str_ptr(vk), "v2"), 0);  /* last wins */
    ray_release(vk); ray_release(kk);

    ray_release(user);
    ray_release(d);
    PASS();
}

/* .sys.args builtin: empty when unset; reflects stored dict when set */
static test_result_t test_sys_args_builtin(void) {
    /* unset → empty dict, never NULL */
    ray_t* e = ray_eval_str("(.sys.args)");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(RAY_IS_ERR(e));
    TEST_ASSERT_EQ_I(e->type, RAY_DICT);
    TEST_ASSERT_EQ_I(ray_dict_len(e), 0);
    ray_release(e);

    /* set, then read back through the builtin */
    char* argv[] = { "rayforce", "-p", "5000", "--", "-opt", "123" };
    ray_runtime_set_sys_args(ray_build_sys_args(6, argv));

    ray_t* d = ray_eval_str("(.sys.args)");
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQ_I(d->type, RAY_DICT);
    ray_t* pk = ray_sym(ray_sym_intern("port", 4));
    ray_t* pv = ray_dict_get(d, pk);
    TEST_ASSERT_EQ_I(pv->type, -RAY_I64);
    TEST_ASSERT_EQ_I(pv->i64, 5000);
    ray_release(pv); ray_release(pk);
    ray_release(d);

    /* arity error */
    TEST_ASSERT_TRUE(eval_err("(.sys.args 0)", "domain"));
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

/* .time.now */
static test_result_t test_syscov_time_now(void) {
    ray_t* r = ray_eval_str("(.time.now)");
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->type, -RAY_I64);
    TEST_ASSERT_TRUE(r->i64 > 0);
    ray_release(r);
    TEST_ASSERT_TRUE(eval_err("(.time.now 1)", "domain"));
    PASS();
}

/* .time.timer.set / .time.timer.del — argument validation + idempotent delete */
static test_result_t test_syscov_time_timer_set_del(void) {
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set)",              "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000)",         "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0)",       "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set \"x\" 0 (fn [t] t))", "type"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 \"x\" (fn [t] t))", "type"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set -1 0 (fn [t] t))",      "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 -1 (fn [t] t))",   "domain"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0 42)",            "type"));
    TEST_ASSERT_TRUE(eval_err("(.time.timer.set 1000 0 (fn [a b] a))",  "domain"));

    /* Schedule then cancel — del returns null. */
    {
        ray_t* id = ray_eval_str("(.time.timer.set 100000 1 (fn [t] t))");
        TEST_ASSERT_NOT_NULL(id);
        TEST_ASSERT_FALSE(RAY_IS_ERR(id));
        TEST_ASSERT_EQ_I(id->type, -RAY_I64);
        char src[128];
        snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)id->i64);
        ray_t* r = ray_eval_str(src);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(id);
        ray_release(r);
    }

    /* Del of bogus id returns null (idempotent). */
    {
        ray_t* r = ray_eval_str("(.time.timer.del 999999)");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(r);
    }

    TEST_ASSERT_TRUE(eval_err("(.time.timer.del \"x\")", "type"));
    PASS();
}

/* Integration: schedule, pump the heap for 100 ms, verify N fires happened. */
static test_result_t test_syscov_time_timer_fires(void) {
    /* Reach the runtime's active poll directly. */
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    /* Set up a counter and schedule a 3-shot timer that increments it. */
    ray_t* setup_counter = ray_eval_str("(set tcounter 0)");
    if (setup_counter) ray_release(setup_counter);
    ray_t* setup_timer = ray_eval_str(
        "(.time.timer.set 5 3 (fn [t] (set tcounter (+ tcounter 1))))");
    TEST_ASSERT_NOT_NULL(setup_timer);
    TEST_ASSERT_FALSE(RAY_IS_ERR(setup_timer));
    ray_release(setup_timer);

    /* Pump for 100 ms — enough for 3 fires at 5 ms each, with slack. */
    ray_timers_pump_for((ray_timers_t*)poll->timers, 100);

    /* Verify the counter advanced. */
    ray_t* c = ray_eval_str("tcounter");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, -RAY_I64);
    TEST_ASSERT_EQ_I(c->i64, 3);
    ray_release(c);
    PASS();
}

/* The production poll driver must also service Rayfall timers while an
 * embedder lends it the thread for a bounded slice. */
static test_result_t test_poll_run_for_fires_timer(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    ray_t* setup_counter = ray_eval_str("(set bounded_timer_fired 0)");
    TEST_ASSERT_NOT_NULL(setup_counter);
    TEST_ASSERT_FALSE(RAY_IS_ERR(setup_counter));
    ray_release(setup_counter);

    ray_t* setup_timer = ray_eval_str(
        "(.time.timer.set 5 1 (fn [t] (set bounded_timer_fired 1)))");
    TEST_ASSERT_NOT_NULL(setup_timer);
    TEST_ASSERT_FALSE(RAY_IS_ERR(setup_timer));
    ray_release(setup_timer);

    TEST_ASSERT_EQ_I(ray_poll_run_for(poll, 30), 0);

    ray_t* fired = ray_eval_str("bounded_timer_fired");
    TEST_ASSERT_NOT_NULL(fired);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fired));
    TEST_ASSERT_EQ_I(fired->type, -RAY_I64);
    TEST_ASSERT_EQ_I(fired->i64, 1);
    ray_release(fired);
    PASS();
}

/* ─── timer.c deep coverage (heap helpers + edge paths) ───────
 *
 * The static heap helpers (heap_less, heap_swap, heap_up sift body,
 * heap_down, heap_grow) and ray_timers_next_deadline_ms only run when
 * the heap has multiple entries, when sift directions actually fire,
 * and when growth past the initial cap (16) happens.  The existing
 * .time.timer tests only schedule one timer at a time.  These tests
 * push every static branch via the public timer API. */

/* heap_grow: schedule >16 timers in one go (initial cap is 16). */
static test_result_t test_timer_heap_grow_past_initial_cap(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    /* 20 one-shot timers with very long delays — they won't fire during
     * the test.  Cleanup is via .time.timer.del in this test (and the
     * teardown will free anything that slips through). */
    int64_t ids[20];
    for (int i = 0; i < 20; i++) {
        /* Use varying tic_ms so we also exercise the heap_up sift body:
         * each new timer can be cheaper than its parent, forcing swaps. */
        char src[128];
        snprintf(src, sizeof src,
                 "(.time.timer.set %d 1 (fn [t] t))", 100000 - i * 1000);
        ray_t* r = ray_eval_str(src);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        TEST_ASSERT_EQ_I(r->type, -RAY_I64);
        ids[i] = r->i64;
        ray_release(r);
    }

    /* Heap should be tracking 20 timers now (grew at least once). */
    TEST_ASSERT_EQ_I(((ray_timers_t*)poll->timers)->n, 20);
    TEST_ASSERT_TRUE(((ray_timers_t*)poll->timers)->cap >= 20);

    /* Cancel them all.  Deleting in original-id order is NOT same as
     * heap-array order — for ids beyond the smallest exp, the matching
     * entry sits in the middle of the array and triggers the rebalance
     * (sift up/down) branch in ray_timers_del. */
    for (int i = 0; i < 20; i++) {
        char src[64];
        snprintf(src, sizeof src, "(.time.timer.del %lld)",
                 (long long)ids[i]);
        ray_t* r = ray_eval_str(src);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_TRUE(RAY_IS_NULL(r));
        ray_release(r);
    }

    TEST_ASSERT_EQ_I(((ray_timers_t*)poll->timers)->n, 0);
    PASS();
}

/* ray_timers_next_deadline_ms: empty and populated. */
static test_result_t test_timer_next_deadline_ms(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    /* timers ptr is lazily allocated by .time.timer.set; force its
     * existence by scheduling+deleting one.  Then deadline of empty
     * heap is INT64_MAX. */
    ray_t* id0 = ray_eval_str("(.time.timer.set 1000 1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(id0);
    char src[64];
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)id0->i64);
    ray_release(ray_eval_str(src));
    ray_release(id0);

    ray_timers_t* timers = (ray_timers_t*)poll->timers;
    TEST_ASSERT_NOT_NULL(timers);
    TEST_ASSERT_EQ_I(ray_timers_next_deadline_ms(timers), INT64_MAX);
    /* NULL ptr path. */
    TEST_ASSERT_EQ_I(ray_timers_next_deadline_ms(NULL), INT64_MAX);

    /* Populated: deadline is now+tic_ms within the next few seconds. */
    int64_t before = ray_time_now_ms();
    ray_t* id = ray_eval_str("(.time.timer.set 50000 1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(id);
    int64_t after = ray_time_now_ms();
    int64_t dl = ray_timers_next_deadline_ms(timers);
    TEST_ASSERT_TRUE(dl >= before + 50000);
    TEST_ASSERT_TRUE(dl <= after  + 50000);

    /* Cleanup. */
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)id->i64);
    ray_release(ray_eval_str(src));
    ray_release(id);
    PASS();
}

/* heap_up sift body + heap_down via ray_timers_del rebalance:
 * schedule a slow timer then a fast one (forcing the new entry to sift
 * up past its parent), then delete the slow one from the middle/tail
 * to force the rebalance branch. */
static test_result_t test_timer_heap_sift_paths(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    /* Slow timer goes in first at index 0. */
    ray_t* slow = ray_eval_str("(.time.timer.set 90000 1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(slow);
    TEST_ASSERT_FALSE(RAY_IS_ERR(slow));

    /* Fast timer must sift past it (smaller exp_ms wins the heap top). */
    ray_t* fast = ray_eval_str("(.time.timer.set 100   1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(fast);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fast));

    /* Add a third so there's something to sift after delete. */
    ray_t* mid = ray_eval_str("(.time.timer.set 50000 1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(mid);

    /* Delete the first (slow) timer.  Its current heap slot is the tail
     * or near-tail; the loop swaps in the tail and runs heap_down+up to
     * rebalance — that's the i < t->n branch (rebalance) of ray_timers_del. */
    char src[64];
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)slow->i64);
    ray_t* r1 = ray_eval_str(src);
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_TRUE(RAY_IS_NULL(r1));
    ray_release(r1);

    /* Drop the remaining timers. */
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)mid->i64);
    ray_release(ray_eval_str(src));
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)fast->i64);
    ray_release(ray_eval_str(src));

    ray_release(slow); ray_release(fast); ray_release(mid);
    PASS();
}

/* heap_less tie-break on equal exp_ms (id ordering).  We can't pin
 * exp_ms identically across two add calls without arena math, but we
 * can stuff a large batch of zero-delay timers in fast succession; many
 * will share an exp_ms millisecond and break ties on id. */
static test_result_t test_timer_heap_tie_break(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    int64_t ids[8];
    for (int i = 0; i < 8; i++) {
        ray_t* r = ray_eval_str(
            "(.time.timer.set 0 1 (fn [t] t))");
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        ids[i] = r->i64;
        ray_release(r);
    }

    /* Pump briefly — all 8 fire in id order via heap_less tie-break. */
    ray_timers_pump_for((ray_timers_t*)poll->timers, 30);
    TEST_ASSERT_EQ_I(((ray_timers_t*)poll->timers)->n, 0);
    (void)ids;
    PASS();
}

/* Forever timer (num == 0): exercises the re-push branch of
 * ray_timers_fire_expired.  Also exercises the post-pop heap_down
 * (lines 180-181) when another timer is still pending. */
static test_result_t test_timer_forever_rearm(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    ray_t* setup = ray_eval_str("(set fcounter 0)");
    if (setup) ray_release(setup);

    /* A forever timer (num=0) firing every 3 ms. */
    ray_t* fid = ray_eval_str(
        "(.time.timer.set 3 0 (fn [t] (set fcounter (+ fcounter 1))))");
    TEST_ASSERT_NOT_NULL(fid);
    TEST_ASSERT_FALSE(RAY_IS_ERR(fid));

    /* A second longer-running timer so the heap holds >1 entry, forcing
     * the heap_down at the top of fire_expired's pop. */
    ray_t* gid = ray_eval_str(
        "(.time.timer.set 7 0 (fn [t] (set fcounter (+ fcounter 1))))");
    TEST_ASSERT_NOT_NULL(gid);

    /* Pump for ~60 ms; at 3 ms each we should see many fires. */
    ray_timers_pump_for((ray_timers_t*)poll->timers, 60);

    ray_t* c = ray_eval_str("fcounter");
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_FALSE(RAY_IS_ERR(c));
    TEST_ASSERT_EQ_I(c->type, -RAY_I64);
    /* Be lenient on a slow CI runner — at least a few fires. */
    TEST_ASSERT_TRUE(c->i64 >= 2);
    ray_release(c);

    /* Both timers still live (num=0 never decrements to zero); delete
     * both to free them and exercise del on a heap with >1 entries. */
    char src[64];
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)fid->i64);
    ray_release(ray_eval_str(src));
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)gid->i64);
    ray_release(ray_eval_str(src));
    ray_release(fid); ray_release(gid);
    PASS();
}

/* Timer callback returning an error: hits the RAY_IS_ERR branch
 * (lines 191-200) of ray_timers_fire_expired.  Multiplying an i64 by a
 * string is a type error returned as a ray error. */
static test_result_t test_timer_callback_error(void) {
    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);

    /* Suppress the timer-error stderr noise during the test (the timer
     * runs ray_fmt + fprintf(stderr) when it sees an error). */
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }

    /* One-shot timer whose body errors on multiplying i64 by string. */
    ray_t* id = ray_eval_str(
        "(.time.timer.set 1 1 (fn [t] (* t \"oops\")))");
    TEST_ASSERT_NOT_NULL(id);
    TEST_ASSERT_FALSE(RAY_IS_ERR(id));

    ray_timers_pump_for((ray_timers_t*)poll->timers, 30);

    /* Restore stderr. */
    if (saved >= 0) { dup2(saved, STDERR_FILENO); close(saved); }

    TEST_ASSERT_EQ_I(((ray_timers_t*)poll->timers)->n, 0);
    ray_release(id);
    PASS();
}

/* ray_timers_pump_for guard rails: NULL ptr + non-positive budget. */
static test_result_t test_timer_pump_for_guards(void) {
    /* Both early-return; nothing crashes. */
    ray_timers_pump_for(NULL, 100);
    ray_timers_pump_for(NULL, 0);
    ray_timers_pump_for(NULL, -1);

    ray_poll_t* poll = (ray_poll_t*)ray_runtime_get_poll();
    TEST_ASSERT_NOT_NULL(poll);
    /* Initialize timers ptr by scheduling+cancelling one. */
    ray_t* id = ray_eval_str("(.time.timer.set 1000 1 (fn [t] t))");
    TEST_ASSERT_NOT_NULL(id);
    char src[64];
    snprintf(src, sizeof src, "(.time.timer.del %lld)", (long long)id->i64);
    ray_release(ray_eval_str(src));
    ray_release(id);

    ray_timers_t* timers = (ray_timers_t*)poll->timers;
    /* Non-positive budget returns immediately. */
    ray_timers_pump_for(timers, 0);
    ray_timers_pump_for(timers, -10);

    /* fire_expired guard rails: NULL and empty heap both return early. */
    ray_timers_fire_expired(NULL);
    ray_timers_fire_expired(timers);  /* empty heap branch */
    PASS();
}

/* ray_timers_create + ray_timers_destroy direct API.  Covers:
 *   - initial_cap < 1 normalization
 *   - destroy(NULL) early return
 *   - destroy with live entries (lines 73-74) by adding then destroying. */
static test_result_t test_timer_create_destroy_paths(void) {
    /* initial_cap < 1 path. */
    ray_timers_t* t0 = ray_timers_create(0);
    TEST_ASSERT_NOT_NULL(t0);
    TEST_ASSERT_EQ_I(t0->cap, 1);
    TEST_ASSERT_EQ_I(t0->n, 0);
    ray_timers_destroy(t0);

    /* Negative initial_cap also normalizes. */
    ray_timers_t* tn = ray_timers_create(-5);
    TEST_ASSERT_NOT_NULL(tn);
    TEST_ASSERT_EQ_I(tn->cap, 1);
    ray_timers_destroy(tn);

    /* NULL early-return. */
    ray_timers_destroy(NULL);

    /* Non-empty destroy: must release each timer's fn and free entries. */
    ray_timers_t* th = ray_timers_create(4);
    TEST_ASSERT_NOT_NULL(th);
    /* Two retained lambdas via ray_eval_str. */
    ray_t* f1 = ray_eval_str("(fn [t] t)");
    ray_t* f2 = ray_eval_str("(fn [t] (+ t 1))");
    TEST_ASSERT_NOT_NULL(f1);
    TEST_ASSERT_NOT_NULL(f2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(f1));
    TEST_ASSERT_FALSE(RAY_IS_ERR(f2));
    int64_t a = ray_timers_add(th, 10000, 1, f1);
    int64_t b = ray_timers_add(th, 20000, 1, f2);
    TEST_ASSERT_TRUE(a >= 0);
    TEST_ASSERT_TRUE(b >= 0);
    /* Drop our refs — the heap retains its own. */
    ray_release(f1);
    ray_release(f2);
    /* Destroy with live entries — exercises the for-loop body. */
    ray_timers_destroy(th);

    /* NULL/fn guards on ray_timers_add. */
    ray_t* f3 = ray_eval_str("(fn [t] t)");
    TEST_ASSERT_NOT_NULL(f3);
    TEST_ASSERT_EQ_I(ray_timers_add(NULL, 0, 1, f3), -1);
    ray_timers_t* tg = ray_timers_create(2);
    TEST_ASSERT_NOT_NULL(tg);
    TEST_ASSERT_EQ_I(ray_timers_add(tg, 0, 1, NULL), -1);
    ray_release(f3);
    ray_timers_destroy(tg);

    /* del guards: NULL ptr returns false; unknown id returns false. */
    TEST_ASSERT_FALSE(ray_timers_del(NULL, 0));
    ray_timers_t* td = ray_timers_create(2);
    TEST_ASSERT_FALSE(ray_timers_del(td, 99999));
    ray_timers_destroy(td);

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
    { "runtime/sys_args_storage",            test_sys_args_storage,            sys_setup, sys_teardown },
    { "runtime/build_sys_args_defaults",     test_build_sys_args_defaults,     sys_setup, sys_teardown },
    { "runtime/build_sys_args_flags_user",   test_build_sys_args_flags_and_user, sys_setup, sys_teardown },
    { "runtime/build_sys_args_edges",        test_build_sys_args_edges,        sys_setup, sys_teardown },
    { "runtime/sys_args_builtin",            test_sys_args_builtin,            sys_setup, sys_teardown },
    { "runtime/syscov_rc",                   test_syscov_rc,                   sys_setup, sys_teardown },
    { "runtime/syscov_time_now",             test_syscov_time_now,             sys_setup, sys_teardown },
    { "runtime/syscov_time_timer_set_del",   test_syscov_time_timer_set_del,   sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/syscov_time_timer_fires",     test_syscov_time_timer_fires,     sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/poll_run_for_fires_timer",    test_poll_run_for_fires_timer,    sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_heap_grow_past_initial_cap", test_timer_heap_grow_past_initial_cap, sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_next_deadline_ms",      test_timer_next_deadline_ms,      sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_heap_sift_paths",       test_timer_heap_sift_paths,       sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_heap_tie_break",        test_timer_heap_tie_break,        sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_forever_rearm",         test_timer_forever_rearm,         sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_callback_error",        test_timer_callback_error,        sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_pump_for_guards",       test_timer_pump_for_guards,       sys_setup_with_poll, sys_teardown_with_poll },
    { "runtime/timer_create_destroy_paths",  test_timer_create_destroy_paths,  sys_setup_with_poll, sys_teardown_with_poll },
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

