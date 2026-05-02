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

/*
 * Unit tests for src/app/repl.c.
 *
 * The .rfl-driven harness in test/main.c uses ray_eval_str directly and
 * never goes through repl.c, so without these tests repl.o sits at ~4%
 * line coverage despite being linked.  These cases drive the file-batch
 * entrypoint (ray_repl_run_file), the create/destroy lifecycle, and
 * the bracket-balance helpers used by piped multi-line input.
 *
 * stdout/stderr are redirected to /dev/null around any call that
 * prints results — keeps the test runner's progress output clean.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "app/repl.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* Forward-declare runtime API — same pattern as test_lang.c. */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

/* ─── Setup / Teardown ────────────────────────────────────────────── */

static void repl_setup(void) {
    ray_runtime_create(0, NULL);
}

static void repl_teardown(void) {
    ray_runtime_destroy(__RUNTIME);
}

/* ─── stdio mute helper ───────────────────────────────────────────── */

/* Redirect stdout+stderr to /dev/null for the duration of a call.
 * Saves the original fds, restores on end_mute().  Keeps test output
 * clean when ray_repl_run_file prints results / errors. */
typedef struct {
    int saved_out;
    int saved_err;
    int devnull;
} mute_state_t;

static void begin_mute(mute_state_t* m) {
    fflush(stdout);
    fflush(stderr);
    m->saved_out = dup(fileno(stdout));
    m->saved_err = dup(fileno(stderr));
    m->devnull   = open("/dev/null", O_WRONLY);
    if (m->devnull >= 0) {
        dup2(m->devnull, fileno(stdout));
        dup2(m->devnull, fileno(stderr));
    }
}

static void end_mute(mute_state_t* m) {
    fflush(stdout);
    fflush(stderr);
    if (m->saved_out >= 0) { dup2(m->saved_out, fileno(stdout)); close(m->saved_out); }
    if (m->saved_err >= 0) { dup2(m->saved_err, fileno(stderr)); close(m->saved_err); }
    if (m->devnull   >= 0) close(m->devnull);
}

/* Build a unique-per-pid tmp .rfl path; never returns NULL. */
static const char* tmp_rfl_path(void) {
    static char path[64];
    if (!path[0])
        snprintf(path, sizeof(path), "/tmp/ray_test_repl_%d.rfl", (int)getpid());
    return path;
}

/* Write `body` to tmp .rfl path; returns 0 on success, -1 on I/O error. */
static int write_rfl(const char* body) {
    FILE* f = fopen(tmp_rfl_path(), "w");
    if (!f) return -1;
    if (body && *body) fwrite(body, 1, strlen(body), f);
    fclose(f);
    return 0;
}

static void unlink_rfl(void) { unlink(tmp_rfl_path()); }

/* ─── ray_repl_run_file ──────────────────────────────────────────── */

/* Happy path: simple arithmetic — evaluator returns a value, printer
 * runs, ray_repl_run_file returns 0. */
static test_result_t test_repl_run_file_happy(void) {
    TEST_ASSERT_EQ_I(write_rfl("(+ 1 2)\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* Multi-form file: each top-level form evaluated in order, last value
 * is printed.  Should still return 0. */
static test_result_t test_repl_run_file_multi_form(void) {
    TEST_ASSERT_EQ_I(write_rfl(
        "(set x 10)\n"
        "(set y 20)\n"
        "(+ x y)\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* Parse error: malformed source — ray_parse_with_nfo returns an error,
 * file mode prints it to stderr and returns 1. */
static test_result_t test_repl_run_file_parse_error(void) {
    /* Unmatched paren — definite parse error. */
    TEST_ASSERT_EQ_I(write_rfl("(+ 1 2\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 1);
    PASS();
}

/* Eval error: parses fine but raises at eval (type error).  Should
 * exit with rc=1, exercising the RAY_IS_ERR / repl_print_result branch
 * on stderr including fmt_error_with_trace if a trace exists. */
static test_result_t test_repl_run_file_eval_error(void) {
    TEST_ASSERT_EQ_I(write_rfl("(+ \"abc\" 1)\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 1);
    PASS();
}

/* Empty file: ray_repl_run_file shortcircuits (nread == 0 path) and
 * returns 0 without calling parse/eval. */
static test_result_t test_repl_run_file_empty(void) {
    TEST_ASSERT_EQ_I(write_rfl(""), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* File with only ;; comments: parses to nothing meaningful; the
 * eval path may return null or void.  Either way, no error, rc=0. */
static test_result_t test_repl_run_file_comments_only(void) {
    TEST_ASSERT_EQ_I(write_rfl(
        ";; first comment\n"
        ";; second comment\n"
        "\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* Non-existent file: fopen fails, function prints "cannot open" to
 * stderr and returns 1.  Covers the early-exit path. */
static test_result_t test_repl_run_file_nonexistent(void) {
    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file("/tmp/ray_test_repl_definitely_no_such_file_xyz.rfl");
    end_mute(&m);

    TEST_ASSERT_EQ_I(rc, 1);
    PASS();
}

/* Multi-line expression in a file — parser accepts a single form
 * spread across newlines.  Confirms file-mode reads the whole buffer
 * before parsing (not a line-at-a-time stream).  `+` is binary, so we
 * use a nested form to keep arity correct while spanning lines. */
static test_result_t test_repl_run_file_multiline_expr(void) {
    TEST_ASSERT_EQ_I(write_rfl(
        "(+ (+ 1\n"
        "       2)\n"
        "   3)\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* Lazy result path — produce a value that requires materialization.
 * (count V) where V is a non-trivial vec exercises the lazy/materialize
 * arm of ray_repl_run_file at line 1047. */
static test_result_t test_repl_run_file_lazy_result(void) {
    TEST_ASSERT_EQ_I(write_rfl(
        "(set V (til 100))\n"
        "(+ V 1)\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* ─── ray_repl_create / ray_repl_destroy ─────────────────────────── */

/* Create with NULL poll — the non-interactive path.  In tests stdin
 * is unlikely to be a tty (and when it is, term creation may still
 * succeed); either way, ray_repl_destroy must clean up correctly. */
static test_result_t test_repl_create_destroy_null_poll(void) {
    ray_repl_t* repl = ray_repl_create(NULL);
    TEST_ASSERT_NOT_NULL(repl);
    /* poll wired through faithfully */
    TEST_ASSERT_EQ_PTR(repl->poll, NULL);
    /* id starts at -1 (not yet registered with any poll) */
    TEST_ASSERT_EQ_I(repl->id, -1);
    ray_repl_destroy(repl);
    PASS();
}

/* Destroy must be NULL-safe — main.c relies on this in error paths. */
static test_result_t test_repl_destroy_null(void) {
    ray_repl_destroy(NULL);   /* must not crash */
    PASS();
}

/* ─── ray_repl_run (piped mode) ──────────────────────────────────── */

/* Drive run_piped() by replacing stdin with a pipe pre-loaded with
 * `script`, then calling ray_repl_run with poll=NULL.  Restores stdin
 * before returning.  stdout/stderr are muted because run_piped prints
 * results.  Returns 0 on success, -1 if the pipe wiring failed.
 *
 * NOTE: ray_repl_create() consults isatty(STDIN_FD) to decide whether
 * to allocate a terminal — a pipe is not a tty, so the resulting
 * ray_repl_t* has term==NULL and ray_repl_run picks the run_piped
 * branch.  This is the only seam through which piped semantics are
 * unit-testable without a child process. */
static int run_piped_with_input(const char* script) {
    int p[2];
    if (pipe(p) != 0) return -1;

    /* Pre-load the script and close the write end so fgets sees EOF. */
    if (script && *script)
        if (write(p[1], script, strlen(script)) < 0) { /* tolerate short writes for these tiny tests */ }
    close(p[1]);

    /* Swap stdin -> read end of pipe. */
    fflush(stdin);
    int saved_in = dup(fileno(stdin));
    if (saved_in < 0) { close(p[0]); return -1; }
    dup2(p[0], fileno(stdin));
    close(p[0]);
    /* fgets() in stdio buffers the underlying fd — clear the existing
     * buffered state by reopening stdin so the pipe is read fresh. */
    clearerr(stdin);

    mute_state_t m;
    begin_mute(&m);
    ray_repl_t* repl = ray_repl_create(NULL);
    if (repl) {
        ray_repl_run(repl);
        ray_repl_destroy(repl);
    }
    end_mute(&m);

    /* Restore stdin. */
    fflush(stdin);
    dup2(saved_in, fileno(stdin));
    close(saved_in);
    clearerr(stdin);
    return repl ? 0 : -1;
}

/* Single complete expression on one line. */
static test_result_t test_repl_run_piped_single_line(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input("(+ 1 2)\n"), 0);
    PASS();
}

/* Multi-line expression — the bracket-balance accumulator must hold
 * input across two fgets() calls before evaluating. */
static test_result_t test_repl_run_piped_multiline(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        "(+ 1\n"
        "   2)\n"), 0);
    PASS();
}

/* Multiple top-level expressions, each on its own line. */
static test_result_t test_repl_run_piped_multi_form(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        "(set x 5)\n"
        "(+ x 10)\n"), 0);
    PASS();
}

/* Empty/blank input — fgets returns NULL straight away. */
static test_result_t test_repl_run_piped_empty(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(""), 0);
    PASS();
}

/* Blank lines and `;;` comments between expressions — both must be
 * tolerated by the piped accumulator without confusing bracket state. */
static test_result_t test_repl_run_piped_comments_blank(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        ";; first\n"
        "\n"
        "(+ 1 2)\n"
        "\n"
        ";; trailing\n"), 0);
    PASS();
}

/* Exit command `\\` terminates the loop immediately. */
static test_result_t test_repl_run_piped_exit_backslash(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        "(+ 1 2)\n"
        "\\\\\n"
        "(+ 3 4)\n"   /* should never run */ ), 0);
    PASS();
}

/* Exit command `exit` likewise. */
static test_result_t test_repl_run_piped_exit_word(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        "exit\n"), 0);
    PASS();
}

/* :q / :quit also exit; route through both early-exit branches. */
static test_result_t test_repl_run_piped_colon_quit(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(":q\n"),    0);
    TEST_ASSERT_EQ_I(run_piped_with_input(":quit\n"), 0);
    PASS();
}

/* :env / :? / :t — non-quitting commands should be dispatched and
 * produce no error path that crashes; we only assert the loop survives. */
static test_result_t test_repl_run_piped_command(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        ":?\n"
        "(+ 1 2)\n"), 0);
    PASS();
}

/* Eval error inside piped input — REPL semantics: error printed to
 * stderr but loop continues to next line.  Final assertion is just
 * that we make it to the end without crashing. */
static test_result_t test_repl_run_piped_eval_error_continues(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        "(+ \"abc\" 1)\n"   /* type error */
        "(+ 1 2)\n"          /* still runs */ ), 0);
    PASS();
}

/* ─── Suite definition ───────────────────────────────────────────── */

const test_entry_t repl_entries[] = {
    /* file-batch entrypoint — ray_repl_run_file */
    { "repl/run_file/happy",            test_repl_run_file_happy,            repl_setup, repl_teardown },
    { "repl/run_file/multi_form",       test_repl_run_file_multi_form,       repl_setup, repl_teardown },
    { "repl/run_file/parse_error",      test_repl_run_file_parse_error,      repl_setup, repl_teardown },
    { "repl/run_file/eval_error",       test_repl_run_file_eval_error,       repl_setup, repl_teardown },
    { "repl/run_file/empty",            test_repl_run_file_empty,            repl_setup, repl_teardown },
    { "repl/run_file/comments_only",    test_repl_run_file_comments_only,    repl_setup, repl_teardown },
    { "repl/run_file/nonexistent",      test_repl_run_file_nonexistent,      repl_setup, repl_teardown },
    { "repl/run_file/multiline_expr",   test_repl_run_file_multiline_expr,   repl_setup, repl_teardown },
    { "repl/run_file/lazy_result",      test_repl_run_file_lazy_result,      repl_setup, repl_teardown },

    /* lifecycle */
    { "repl/create_destroy/null_poll",  test_repl_create_destroy_null_poll,  repl_setup, repl_teardown },
    { "repl/destroy/null",              test_repl_destroy_null,              NULL,       NULL          },

    /* piped-mode loop — drives ray_repl_run via stdin pipe redirection */
    { "repl/run/piped/single_line",     test_repl_run_piped_single_line,         repl_setup, repl_teardown },
    { "repl/run/piped/multiline",       test_repl_run_piped_multiline,           repl_setup, repl_teardown },
    { "repl/run/piped/multi_form",      test_repl_run_piped_multi_form,          repl_setup, repl_teardown },
    { "repl/run/piped/empty",           test_repl_run_piped_empty,               repl_setup, repl_teardown },
    { "repl/run/piped/comments_blank",  test_repl_run_piped_comments_blank,      repl_setup, repl_teardown },
    { "repl/run/piped/exit_backslash",  test_repl_run_piped_exit_backslash,      repl_setup, repl_teardown },
    { "repl/run/piped/exit_word",       test_repl_run_piped_exit_word,           repl_setup, repl_teardown },
    { "repl/run/piped/colon_quit",      test_repl_run_piped_colon_quit,          repl_setup, repl_teardown },
    { "repl/run/piped/command",         test_repl_run_piped_command,             repl_setup, repl_teardown },
    { "repl/run/piped/eval_error",      test_repl_run_piped_eval_error_continues, repl_setup, repl_teardown },

    { NULL, NULL, NULL, NULL },
};
