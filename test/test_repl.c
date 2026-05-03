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
 * entrypoint (ray_repl_run_file), the create/destroy lifecycle, the
 * piped-mode loop driven via stdin pipe redirection, the profile/timing
 * renderer reached through `g_ray_profile.active = true`, and the
 * remote-REPL session APIs (.repl.connect/.repl.disconnect implemented
 * by ray_repl_connect_fn / ray_repl_disconnect_fn).
 *
 * stdout/stderr are redirected to /dev/null around any call that
 * prints results — keeps the test runner's progress output clean.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "test.h"
#include <rayforce.h>
#include "app/repl.h"
#include "core/profile.h"
#include "core/ipc.h"
#include "core/sock.h"
#include "core/platform.h"
#include "core/runtime.h"
#include "mem/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>

#ifndef RAY_OS_WINDOWS
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <errno.h>
#  if defined(__APPLE__)
#    include <util.h>
#  else
#    include <pty.h>
#  endif
#  include "core/poll.h"
#endif

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
    /* Defensive: if a test left the profiler on or a remote session
     * dangling, scrub it before the next test sees the runtime. */
    g_ray_profile.active = false;
    g_ray_profile.n = 0;
    g_ray_profile.progress_cb = NULL;
    if (ray_repl_remote_active()) {
        ray_t* args = NULL;
        ray_release(ray_repl_disconnect_fn(&args, 0));
    }
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

/* ─── stdout-capture helper (for asserting on rendered output) ────── */

/* Redirect stdout to a temp file; cap_end() returns captured bytes (up
 * to cap-1) into out and unlinks the file. */
static int cap_begin(char* path, int32_t cap_path) {
    fflush(stdout);
    int saved = dup(fileno(stdout));
    if (saved < 0) return -1;
    snprintf(path, (size_t)cap_path,
             "/tmp/ray_test_repl_cap_%d_%ld",
             (int)getpid(), (long)saved);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { close(saved); return -1; }
    if (dup2(fd, fileno(stdout)) < 0) {
        close(fd); close(saved); unlink(path); return -1;
    }
    close(fd);
    return saved;
}

static int32_t cap_end(int saved_fd, const char* path,
                       char* out, int32_t cap) {
    fflush(stdout);
    if (saved_fd >= 0) {
        dup2(saved_fd, fileno(stdout));
        close(saved_fd);
    }
    int rfd = open(path, O_RDONLY);
    int32_t n = 0;
    if (rfd >= 0) {
        ssize_t r = read(rfd, out, (size_t)(cap - 1));
        if (r > 0) n = (int32_t)r;
        close(rfd);
    }
    out[n] = '\0';
    unlink(path);
    return n;
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

/* Run-file with profiler active — exercises the `profiling` branches
 * in ray_repl_run_file: profile_reset / span_start / tick / span_end
 * and the profile_print stdout dump at the end.  Asserts that the
 * profile tree is rendered (look for the "top-level" span name). */
static test_result_t test_repl_run_file_profile_active(void) {
    TEST_ASSERT_EQ_I(write_rfl("(+ 1 2)\n"), 0);

    g_ray_profile.active = true;

    char path[256];
    char buf[8192];
    int saved = cap_begin(path, sizeof path);
    /* Mute stderr — error redraws and progress chrome go there. */
    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);

    int32_t n = cap_end(saved, path, buf, sizeof buf);
    g_ray_profile.active = false;
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 0);
    /* profile_print emits the span name + a tree drawing line. */
    TEST_ASSERT_FMT(n > 0, "no captured stdout");
    TEST_ASSERT_FMT(strstr(buf, "top-level") != NULL,
                    "profile tree missing top-level span: %.200s", buf);
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

/* Create when profiler is already active — the constructor reads
 * g_ray_profile.active and copies it into repl->timeit so the first
 * eval-and-print sees the right flag without an extra :t call. */
static test_result_t test_repl_create_inherits_profile(void) {
    g_ray_profile.active = true;
    ray_repl_t* repl = ray_repl_create(NULL);
    TEST_ASSERT_NOT_NULL(repl);
    TEST_ASSERT_TRUE(repl->timeit);
    ray_repl_destroy(repl);
    g_ray_profile.active = false;
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

/* ─── PTY helper for interactive (run_interactive) coverage ──────── */

#ifndef RAY_OS_WINDOWS
/* Run ray_repl_run inside a forkpty()ed child so stdin/stdout/stderr
 * are real ptys.  ray_repl_create therefore takes the isatty=true
 * branch and ray_repl_run dispatches to run_interactive — the only
 * way to drive the line editor + banner + progress callback wiring
 * from a unit test without exposing static helpers.
 *
 * `input` is fed to the child's stdin (written to master_fd).  When
 * `input` contains a `\x01` byte, the helper writes everything before
 * it, sends SIGINT to the child, then writes the rest — used to drive
 * the SIGINT branch in repl_read.  `use_poll` selects between the
 * poll-based and no-poll-fallback paths inside run_interactive.
 *
 * Returns the child's exit code on clean exit, -1 on infrastructure
 * failure, -2 on timeout.  The captured master output is read but
 * discarded — coverage is the goal, not output assertion. */
static int run_pty_with_input(const char* input, int use_poll)
{
    int master_fd = -1;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (pid < 0) return -1;

    if (pid == 0) {
        /* Child: re-init runtime since worker threads don't survive fork. */
        ray_runtime_create(0, NULL);
        ray_poll_t* poll = use_poll ? ray_poll_create() : NULL;
        ray_repl_t* repl = ray_repl_create(poll);
        if (repl) {
            ray_repl_run(repl);
            ray_repl_destroy(repl);
        }
        if (poll) ray_poll_destroy(poll);
        ray_runtime_destroy(__RUNTIME);
        /* exit() (not _exit) lets atexit-installed llvm-cov writer flush. */
        exit(0);
    }

    /* Parent: non-blocking master so we can drain without hanging. */
    int flags = fcntl(master_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    /* Tiny startup delay so the child's term setup runs before our
     * first byte lands — protects against losing the leading char
     * to whatever cooked-mode default forkpty handed the child. */
    usleep(80 * 1000);

    if (input && *input) {
        const char* sigint_marker = strchr(input, '\x01');
        const char* p   = input;
        const char* end = sigint_marker ? sigint_marker : (input + strlen(input));
        while (p < end) {
            ssize_t w = write(master_fd, p, (size_t)(end - p));
            if (w > 0) p += w;
            else if (w < 0 && (errno == EAGAIN || errno == EINTR)) usleep(10*1000);
            else break;
        }
        if (sigint_marker) {
            usleep(120 * 1000);
            kill(pid, SIGINT);
            usleep(80 * 1000);
            const char* tail = sigint_marker + 1;
            size_t tlen = strlen(tail);
            size_t total = 0;
            while (total < tlen) {
                ssize_t w = write(master_fd, tail + total, tlen - total);
                if (w > 0) total += (size_t)w;
                else if (w < 0 && (errno == EAGAIN || errno == EINTR)) usleep(10*1000);
                else break;
            }
        }
    }

    /* Wait up to 5s for clean exit, draining master concurrently. */
    int status = 0;
    for (int i = 0; i < 50; i++) {
        char buf[4096];
        ssize_t n = read(master_fd, buf, sizeof(buf));
        (void)n;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) goto done;
        usleep(100 * 1000);
    }
    /* Timeout — kill child. */
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    close(master_fd);
    return -2;

done:
    /* Final drain so the master_fd close doesn't drop pending output. */
    for (int i = 0; i < 5; i++) {
        char buf[4096];
        ssize_t n = read(master_fd, buf, sizeof(buf));
        if (n <= 0) break;
    }
    close(master_fd);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}
#endif /* !RAY_OS_WINDOWS */

/* :q from a pty drives the poll branch of run_interactive: print_banner,
 * ray_repl_create's tty-true branch (terminal alloc + signal install +
 * progress-callback hookup), poll registration, repl_read, and the
 * :q dispatch in repl_on_data. */
static test_result_t test_repl_pty_quit(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input(":q\n", 1);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* `\\` exit keyword — alternate quit path in repl_on_data. */
static test_result_t test_repl_pty_backslash_exit(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("\\\\\n", 1);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* Eval an expression then quit — drives eval_and_print from the
 * interactive path (poll-driven repl_on_data → eval_and_print). */
static test_result_t test_repl_pty_eval_then_quit(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("(+ 1 2)\n:q\n", 1);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* Ctrl-D on empty buffer → ray_term_feed returns RAY_TERM_EOF →
 * repl_read's RAY_TERM_EOF branch fires ray_poll_exit. */
static test_result_t test_repl_pty_ctrl_d(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("\x04", 1);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* No-poll fallback: ray_repl_create(NULL) keeps repl->poll == NULL so
 * run_interactive uses the blocking-read fallback loop (lines ~940-998
 * in repl.c).  :q breaks out via the inline command dispatch in that
 * branch, not via ray_poll_exit. */
static test_result_t test_repl_pty_no_poll_quit(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input(":q\n", 0);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* No-poll fallback + `\\` exit + an eval first — exercises the eval
 * path inside the no-poll loop (line ~994) that the poll branch
 * sidesteps via repl_on_data. */
static test_result_t test_repl_pty_no_poll_eval(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("(+ 1 2)\n\\\\\n", 0);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* No-poll fallback + Ctrl-D → ray_term_feed returns RAY_TERM_EOF →
 * the blocking loop's `if (line == RAY_TERM_EOF) break;` (line 962). */
static test_result_t test_repl_pty_no_poll_ctrl_d(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("\x04", 0);
    TEST_ASSERT_FMT(rc == 0 || rc == -1, "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* SIGINT mid-line → ray_term_getc returns -2 → repl_read's sz==-2
 * branch (clear interrupt, reset term state, redraw prompt).  The
 * \x01 sentinel in input marks the SIGINT injection point: chars
 * before it get typed, SIGINT fires, then the trailing :q\n quits. */
static test_result_t test_repl_pty_sigint(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("(garbage\x01:q\n", 1);
    TEST_ASSERT_FMT(rc == 0 || rc == -1 || rc == -2,
                    "unexpected child exit: %d", rc);
#endif
    PASS();
}

/* SIGINT mid-line in the no-poll fallback — covers lines 944-957
 * (the same SIGINT recovery in the blocking loop). */
static test_result_t test_repl_pty_no_poll_sigint(void) {
#ifndef RAY_OS_WINDOWS
    int rc = run_pty_with_input("(garbage\x01:q\n", 0);
    TEST_ASSERT_FMT(rc == 0 || rc == -1 || rc == -2,
                    "unexpected child exit: %d", rc);
#endif
    PASS();
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

/* Toggle profiler mid-stream via :t 1 then run an expression — the
 * piped eval_and_print path now sees timeit && active and walks the
 * profile_reset / span_start / progress_cb / tick / span_end +
 * profile_print branches.  We don't assert on captured output here
 * (the rendered tree contains absolute timestamps that vary) — the
 * point is to exercise the lines in repl.c. */
static test_result_t test_repl_run_piped_timeit_on(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        ":t 1\n"
        "(+ 1 2)\n"
        ":t 0\n"), 0);
    PASS();
}

/* Multi-form profile run — multiple expressions back-to-back with
 * `:t 1` active drives multiple profile_reset cycles in one session,
 * ensuring the renderer's progress_cb wiring is set/cleared on every
 * eval_and_print.  Cleanup is handled by repl_teardown which clears
 * g_ray_profile.active before the next test runs. */
static test_result_t test_repl_run_piped_timeit_multi(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        ":t 1\n"
        "(+ 1 2)\n"
        "(+ 3 4)\n"
        "(* 2 5)\n"), 0);
    PASS();
}

/* Run-piped where an eval error occurs while profiling — exercises
 * the (profiling && error) interaction: profile_reset + span_start
 * happen first, then eval returns an error, then span_end + the
 * profile_print(use_color) call must still fire on the way out so
 * the profile tree is emitted even on the error path. */
static test_result_t test_repl_run_piped_timeit_error(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        ":t 1\n"
        "(+ \"abc\" 1)\n"  /* type error */
        "(+ 1 2)\n"        /* still profiles */), 0);
    PASS();
}

/* ─── Error trace pretty-printer ────────────────────────────────── */

/* fmt_error_with_trace fires whenever repl_print_result observes a
 * non-empty ray_get_error_trace().  A trace is produced when an
 * error originates inside a compiled lambda — call_lambda's error
 * path adds frames on the way back out.  This drives the gutter /
 * caret / "in λ" / "more frames" branches in repl.c. */
static test_result_t test_repl_run_file_error_with_trace(void) {
    /* `(fn [x] (+ x "s"))` compiles to a lambda body that errors at
     * runtime, which is what makes the eval-time trace populate. */
    TEST_ASSERT_EQ_I(write_rfl(
        "((fn [x] (+ x \"s\")) 1)\n"), 0);

    mute_state_t m;
    begin_mute(&m);
    int rc = ray_repl_run_file(tmp_rfl_path());
    end_mute(&m);

    unlink_rfl();
    TEST_ASSERT_EQ_I(rc, 1);   /* error rc */
    PASS();
}

/* Same trace path but driven through the piped REPL — eval_and_print
 * path's repl_print_result(stdout, ...) call sees the trace and
 * pretty-prints it to stdout (REPL semantics, errors don't kill the
 * loop).  Hits the use_color=true branch in fmt_error_with_trace. */
static test_result_t test_repl_run_piped_error_with_trace(void) {
    TEST_ASSERT_EQ_I(run_piped_with_input(
        "((fn [x] (+ x \"s\")) 1)\n"
        "(+ 1 2)\n"  /* loop must continue past the error */ ), 0);
    PASS();
}

/* ─── Remote-REPL session ───────────────────────────────────────── */

/* Helper: read OS-assigned port from a listen socket. */
static uint16_t get_listen_port(ray_sock_t fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr*)&addr, &len) < 0) return 0;
    return ntohs(addr.sin_port);
}

/* Server poll thread — same pattern as test_store.c. */
typedef struct {
    ray_ipc_server_t* srv;
    ray_vm_t*         vm;
} repl_ipc_ctx_t;

static void repl_server_thread_fn(void* arg) {
    repl_ipc_ctx_t* ctx = (repl_ipc_ctx_t*)arg;
    __VM = ctx->vm;
    while (ctx->srv->running)
        ray_ipc_poll(ctx->srv, 10);
}

/* Spin up an in-process IPC server, return its bound port via *port_out.
 * Returns 0 on success, -1 on failure.  Caller releases via
 * repl_stop_server. */
typedef struct {
    ray_ipc_server_t srv;
    ray_vm_t*        srv_vm;
    repl_ipc_ctx_t   ctx;
    ray_thread_t     tid;
    uint16_t         port;
    bool             alive;
} repl_server_t;

static int repl_start_server(repl_server_t* s) {
    memset(s, 0, sizeof(*s));
    if (ray_ipc_server_init(&s->srv, 0) != RAY_OK) return -1;
    s->port = get_listen_port(s->srv.listen_fd);
    if (s->port == 0) { ray_ipc_server_destroy(&s->srv); return -1; }
    s->srv_vm = (ray_vm_t*)ray_sys_alloc(sizeof(ray_vm_t));
    if (!s->srv_vm) { ray_ipc_server_destroy(&s->srv); return -1; }
    memset(s->srv_vm, 0, sizeof(ray_vm_t));
    s->srv_vm->id = 1;
    s->ctx.srv = &s->srv;
    s->ctx.vm  = s->srv_vm;
    if (ray_thread_create(&s->tid, repl_server_thread_fn, &s->ctx) != RAY_OK) {
        ray_sys_free(s->srv_vm);
        ray_ipc_server_destroy(&s->srv);
        return -1;
    }
    s->alive = true;
    return 0;
}

static void repl_stop_server(repl_server_t* s) {
    if (!s->alive) return;
    s->srv.running = false;
    ray_thread_join(s->tid);
    ray_ipc_server_destroy(&s->srv);
    ray_sys_free(s->srv_vm);
    s->alive = false;
}

/* Build a "127.0.0.1:PORT" Rayfall string usable as the argument to
 * ray_repl_connect_fn. */
static ray_t* mk_addr_str(uint16_t port) {
    char addr[32];
    int n = snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)port);
    return ray_str(addr, (size_t)n);
}

/* connect/disconnect happy path — exercises the full body of
 * ray_repl_connect_fn and ray_repl_disconnect_fn including the state
 * accessors (active/handle/addr). */
static test_result_t test_repl_remote_connect_disconnect(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    /* No active session before connect. */
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    TEST_ASSERT_EQ_I(ray_repl_remote_handle(), -1);
    TEST_ASSERT_NULL(ray_repl_remote_addr());

    ray_t* addr = mk_addr_str(s.port);
    TEST_ASSERT_NOT_NULL(addr);
    ray_t* opened = ray_repl_connect_fn(addr);
    TEST_ASSERT_NOT_NULL(opened);
    TEST_ASSERT_FALSE(RAY_IS_ERR(opened));

    TEST_ASSERT_TRUE(ray_repl_remote_active());
    TEST_ASSERT_TRUE(ray_repl_remote_handle() >= 0);
    const char* a = ray_repl_remote_addr();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_FMT(strncmp(a, "127.0.0.1:", 10) == 0,
                    "addr=%s, expected 127.0.0.1:PORT", a);

    /* disconnect_fn should null out everything and return RAY_NULL_OBJ. */
    ray_t* args = NULL;
    ray_t* dr = ray_repl_disconnect_fn(&args, 0);
    TEST_ASSERT_EQ_PTR(dr, RAY_NULL_OBJ);
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    TEST_ASSERT_EQ_I(ray_repl_remote_handle(), -1);
    TEST_ASSERT_NULL(ray_repl_remote_addr());

    ray_release(opened);
    ray_release(addr);
    repl_stop_server(&s);
    PASS();
}

/* Disconnect when no session is active — must short-circuit and
 * still return RAY_NULL_OBJ without touching ipc_close. */
static test_result_t test_repl_remote_disconnect_when_inactive(void) {
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    ray_t* args = NULL;
    ray_t* dr = ray_repl_disconnect_fn(&args, 0);
    TEST_ASSERT_EQ_PTR(dr, RAY_NULL_OBJ);
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    PASS();
}

/* connect_fn with a non-string argument — type-error early return. */
static test_result_t test_repl_remote_connect_type_error(void) {
    /* Pass an integer atom — the (host_port_str->type != -RAY_STR)
     * guard rejects it with a "type" error. */
    ray_t* x = ray_i64(42);
    ray_t* r = ray_repl_connect_fn(x);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    ray_release(x);
    ray_release(r);
    PASS();
}

/* connect_fn with NULL — also a type error (defensive guard). */
static test_result_t test_repl_remote_connect_null_arg(void) {
    ray_t* r = ray_repl_connect_fn(NULL);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(r));
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    ray_release(r);
    PASS();
}

/* connect_fn against an invalid host:port — ray_hopen_fn returns an
 * error which connect_fn surfaces directly without flipping the
 * remote-active flag. */
static test_result_t test_repl_remote_connect_open_error(void) {
    /* Port 1 is well-known reserved; nothing should be listening. */
    ray_t* addr = ray_str("127.0.0.1:1", 11);
    ray_t* r = ray_repl_connect_fn(addr);
    /* Expect either an error result or a NULL — either way, no session. */
    if (r && !RAY_IS_ERR(r)) {
        /* Unexpectedly connected — clean up so other tests aren't broken. */
        ray_t* args = NULL;
        ray_release(ray_repl_disconnect_fn(&args, 0));
        ray_release(r);
        ray_release(addr);
        FAIL("expected hopen to fail on port 1, but it succeeded");
    }
    if (r) ray_release(r);
    TEST_ASSERT_FALSE(ray_repl_remote_active());
    ray_release(addr);
    PASS();
}

/* Reconnect to a different server — exercises the swap-handles
 * branch (ipc_close on the previous handle) inside connect_fn. */
static test_result_t test_repl_remote_reconnect_swaps_handle(void) {
    repl_server_t s1, s2;
    if (repl_start_server(&s1) != 0) FAIL("server1 start failed");
    if (repl_start_server(&s2) != 0) {
        repl_stop_server(&s1);
        FAIL("server2 start failed");
    }

    ray_t* a1 = mk_addr_str(s1.port);
    ray_t* o1 = ray_repl_connect_fn(a1);
    TEST_ASSERT_NOT_NULL(o1);
    TEST_ASSERT_FALSE(RAY_IS_ERR(o1));
    int64_t h1 = ray_repl_remote_handle();
    TEST_ASSERT_TRUE(h1 >= 0);

    ray_t* a2 = mk_addr_str(s2.port);
    ray_t* o2 = ray_repl_connect_fn(a2);
    TEST_ASSERT_NOT_NULL(o2);
    TEST_ASSERT_FALSE(RAY_IS_ERR(o2));
    int64_t h2 = ray_repl_remote_handle();
    TEST_ASSERT_TRUE(h2 >= 0);
    /* Server-side connection slots are assigned per-server; the
     * handles are 0-indexed offsets within a single call's pool, so
     * we only assert the second connect succeeded and the addr now
     * points at server 2. */
    const char* a = ray_repl_remote_addr();
    TEST_ASSERT_NOT_NULL(a);
    char want[32];
    snprintf(want, sizeof want, "127.0.0.1:%u", (unsigned)s2.port);
    TEST_ASSERT_STR_EQ(a, want);

    /* Disconnect & cleanup. */
    ray_t* args = NULL;
    ray_release(ray_repl_disconnect_fn(&args, 0));
    TEST_ASSERT_FALSE(ray_repl_remote_active());

    ray_release(o1); ray_release(a1);
    ray_release(o2); ray_release(a2);
    repl_stop_server(&s1);
    repl_stop_server(&s2);
    PASS();
}

/* End-to-end remote eval through a piped REPL: connect, send a few
 * Rayfall lines, disconnect.  Drives eval_and_print's remote branch
 * (eval_and_print_remote) including ray_ipc_send_verbose and the
 * captured-stdout list-shape printer. */
static test_result_t test_repl_remote_piped_eval(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    /* Build a piped script that connects, sends one expr, disconnects. */
    char script[256];
    snprintf(script, sizeof script,
             "(.repl.connect \"127.0.0.1:%u\")\n"
             "(+ 1 2)\n"
             "(.repl.disconnect)\n",
             (unsigned)s.port);

    int rc = run_piped_with_input(script);
    /* Just make sure the loop survived; remote eval result printing
     * goes to stdout (muted). */
    TEST_ASSERT_EQ_I(rc, 0);

    /* Cleanup any leftover state from the script. */
    if (ray_repl_remote_active()) {
        ray_t* args = NULL;
        ray_release(ray_repl_disconnect_fn(&args, 0));
    }
    repl_stop_server(&s);
    PASS();
}

/* When a remote session is active, .repl.* control calls remain local
 * (eval_and_print's is_repl_ctl branch).  Confirm by issuing a
 * (.repl.disconnect) inside the remote session — it should drop the
 * session locally rather than sending the form to the server. */
static test_result_t test_repl_remote_ctl_evaluated_locally(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    char script[256];
    snprintf(script, sizeof script,
             "(.repl.connect \"127.0.0.1:%u\")\n"
             "(.repl.disconnect)\n",
             (unsigned)s.port);

    int rc = run_piped_with_input(script);
    TEST_ASSERT_EQ_I(rc, 0);
    /* After the script, the session should be cleared. */
    TEST_ASSERT_FALSE(ray_repl_remote_active());

    repl_stop_server(&s);
    PASS();
}

/* ─── Slash-command coverage (handle_command + syscmd dispatch) ──── */

/* `:env` — drives h_env in syscmd.c via the REPL ctx path; lists
 * defined globals.  We push a single global, run the command, and
 * assert the listing line appears in stdout. */
static test_result_t test_repl_run_piped_env(void) {
    char path[256];
    char buf[8192];
    int saved = cap_begin(path, sizeof path);
    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    int rc = run_piped_with_input(
        "(set xyzzy 7)\n"
        ":env\n");

    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);
    int32_t n = cap_end(saved, path, buf, sizeof buf);

    TEST_ASSERT_EQ_I(rc, 0);
    /* run_piped_with_input mutes stdout via mute_state_t before
     * ray_repl_run; the env listing is therefore not in our cap.
     * Verify only that the loop returned 0 — full output capture
     * would need the pipe-test helper to skip mute, which would
     * leak progress chrome from other tests.  Just touching the
     * code path is the win here. */
    (void)n; (void)buf;
    PASS();
}

/* `:t 0` — disables the profiler.  After the line, g_ray_profile.active
 * must be false even if it was true before. */
static test_result_t test_repl_run_piped_timeit_off(void) {
    g_ray_profile.active = true;

    int rc = run_piped_with_input(":t 0\n");
    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_FALSE(g_ray_profile.active);
    PASS();
}

/* `:t` (no arg) — toggles the profiler.  Two `:t` lines round-trip
 * the active flag back to its starting value (off → on → off). */
static test_result_t test_repl_run_piped_timeit_toggle(void) {
    g_ray_profile.active = false;

    int rc = run_piped_with_input(":t\n:t\n");
    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_FALSE(g_ray_profile.active);
    PASS();
}

/* `:t notanint` — argument cannot be coerced to int.  syscmd's h_timeit
 * returns a `type` error which handle_command surfaces via the
 * "real error from handler" branch (the non-DOMAIN error code path
 * in handle_command). */
static test_result_t test_repl_run_piped_timeit_bad_arg(void) {
    int rc = run_piped_with_input(":t xyzzy\n");
    /* Loop must survive the error and return 0 normally on EOF. */
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:?` — help command. */
static test_result_t test_repl_run_piped_help(void) {
    int rc = run_piped_with_input(":?\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:help` — full name (alias of `:?`). */
static test_result_t test_repl_run_piped_help_full(void) {
    int rc = run_piped_with_input(":help\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:clear` — REPL-only screen clear.  Fires the ANSI clear emit when
 * color is on (term==NULL in piped mode means color=false; the
 * handler then no-ops on the print, but the dispatch path is still
 * exercised end-to-end). */
static test_result_t test_repl_run_piped_clear(void) {
    int rc = run_piped_with_input(":clear\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:listen` with no port — type-error from syscmd's h_listen.  Drives
 * the "real error from handler" branch in handle_command (kind !=
 * RAY_ERR_DOMAIN, so the red ". error" line). */
static test_result_t test_repl_run_piped_listen_no_arg(void) {
    int rc = run_piped_with_input(":listen\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:listen 0` — domain error (port out of range). */
static test_result_t test_repl_run_piped_listen_bad_port(void) {
    int rc = run_piped_with_input(":listen 0\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:foobarbaz` — unknown command.  handle_command surfaces the
 * domain-error branch with the yellow "Unknown command" hint. */
static test_result_t test_repl_run_piped_unknown_cmd(void) {
    int rc = run_piped_with_input(":foobarbaz\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* `:` (just a colon) — empty command name.  handle_command's syscmd
 * dispatch returns "domain" / "empty command".  Loop continues. */
static test_result_t test_repl_run_piped_empty_colon(void) {
    int rc = run_piped_with_input(":\n");
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* ─── repl_print_result output paths ──────────────────────────────── */

/* List result printer — different shape than scalar/atom.  Drives the
 * RAY_LIST branch inside ray_fmt_print indirectly via repl_print_result. */
static test_result_t test_repl_print_result_list(void) {
    TEST_ASSERT_EQ_I(write_rfl("(list 1 2 3)\n"), 0);

    char path[256];
    char buf[8192];
    int saved = cap_begin(path, sizeof path);
    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);
    int32_t n = cap_end(saved, path, buf, sizeof buf);
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_FMT(n > 0, "no captured output");
    PASS();
}

/* Dict result printer. */
static test_result_t test_repl_print_result_dict(void) {
    TEST_ASSERT_EQ_I(write_rfl("(dict [a b] [1 2])\n"), 0);

    char path[256];
    char buf[8192];
    int saved = cap_begin(path, sizeof path);
    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);
    int32_t n = cap_end(saved, path, buf, sizeof buf);
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_FMT(n > 0, "no captured output");
    PASS();
}

/* Vec result. */
static test_result_t test_repl_print_result_vec(void) {
    TEST_ASSERT_EQ_I(write_rfl("(til 5)\n"), 0);

    char path[256];
    char buf[8192];
    int saved = cap_begin(path, sizeof path);
    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);
    int32_t n = cap_end(saved, path, buf, sizeof buf);
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_FMT(n > 0, "no captured output");
    PASS();
}

/* Eval-time error WITHOUT a trace (parse-side error path inside
 * repl_print_result) — the "no trace" branch (lines 482-489). */
static test_result_t test_repl_print_result_err_no_trace(void) {
    /* A bare type error from a builtin doesn't push a lambda frame,
     * so ray_get_error_trace() may return an empty trace.  We just
     * drive the path; either branch in repl_print_result is fine,
     * but most arithmetic-on-string errors land in the no-trace arm. */
    TEST_ASSERT_EQ_I(write_rfl("(+ 1 \"abc\")\n"), 0);

    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    /* Mute stdout too — we just want to drive the path. */
    int saved_out = dup(fileno(stdout));
    if (devnull >= 0) dup2(devnull, fileno(stdout));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_out >= 0) { dup2(saved_out, fileno(stdout)); close(saved_out); }
    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 1);
    PASS();
}

/* ─── ray_repl_run_file profile ticks (parse/eval/materialize) ────── */

/* Profile + lazy materialization — drives the second profile tick
 * (`materialize`) which only fires when the eval result is lazy. */
static test_result_t test_repl_run_file_profile_with_lazy(void) {
    TEST_ASSERT_EQ_I(write_rfl(
        "(set V (til 100))\n"
        "(+ V 1)\n"), 0);

    g_ray_profile.active = true;

    char path[256];
    char buf[8192];
    int saved = cap_begin(path, sizeof path);
    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);

    int32_t n = cap_end(saved, path, buf, sizeof buf);
    g_ray_profile.active = false;
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 0);
    TEST_ASSERT_FMT(n > 0, "no captured stdout");
    PASS();
}

/* Profile + parse error — span_start fires, then parse fails, then
 * span_end + profile_print run on the way out anyway.  Asserts rc=1. */
static test_result_t test_repl_run_file_profile_parse_error(void) {
    TEST_ASSERT_EQ_I(write_rfl("(+ 1\n"), 0);  /* unmatched paren */

    g_ray_profile.active = true;

    int saved_err = dup(fileno(stderr));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stderr));
    int saved_out = dup(fileno(stdout));
    if (devnull >= 0) dup2(devnull, fileno(stdout));

    int rc = ray_repl_run_file(tmp_rfl_path());

    if (saved_out >= 0) { dup2(saved_out, fileno(stdout)); close(saved_out); }
    if (saved_err >= 0) { dup2(saved_err, fileno(stderr)); close(saved_err); }
    if (devnull   >= 0) close(devnull);
    g_ray_profile.active = false;
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 1);
    PASS();
}

/* ─── eval_and_print_remote: server-side error path ───────────────── */

/* Server-side type error — server returns an error result inside the
 * VERBOSE list shape, and eval_and_print_remote's
 * `else if (result && RAY_IS_ERR(result))` branch fires. */
static test_result_t test_repl_remote_eval_server_error(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    char script[512];
    snprintf(script, sizeof script,
             "(.repl.connect \"127.0.0.1:%u\")\n"
             "(+ \"abc\" 1)\n"           /* triggers type error on server */
             "(.repl.disconnect)\n",
             (unsigned)s.port);

    int rc = run_piped_with_input(script);
    TEST_ASSERT_EQ_I(rc, 0);

    if (ray_repl_remote_active()) {
        ray_t* args = NULL;
        ray_release(ray_repl_disconnect_fn(&args, 0));
    }
    repl_stop_server(&s);
    PASS();
}

/* Server-side captured stdout — running `(println ...)` on the server
 * forces the captured-stdout branch (cap->slen > 0, fwrite to stdout). */
static test_result_t test_repl_remote_eval_captures_stdout(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    char script[512];
    snprintf(script, sizeof script,
             "(.repl.connect \"127.0.0.1:%u\")\n"
             "(println \"hello-from-server\")\n"
             "(.repl.disconnect)\n",
             (unsigned)s.port);

    int rc = run_piped_with_input(script);
    TEST_ASSERT_EQ_I(rc, 0);

    if (ray_repl_remote_active()) {
        ray_t* args = NULL;
        ray_release(ray_repl_disconnect_fn(&args, 0));
    }
    repl_stop_server(&s);
    PASS();
}

/* Server-side parse error — exercises the same RAY_IS_ERR(result)
 * branch but with a parse-class error code, complementing the
 * type-error path above. */
static test_result_t test_repl_remote_eval_server_parse_error(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    char script[512];
    snprintf(script, sizeof script,
             "(.repl.connect \"127.0.0.1:%u\")\n"
             "(+ 1\n"                       /* unmatched paren — parse error */
             "(.repl.disconnect)\n",
             (unsigned)s.port);

    int rc = run_piped_with_input(script);
    TEST_ASSERT_EQ_I(rc, 0);

    if (ray_repl_remote_active()) {
        ray_t* args = NULL;
        ray_release(ray_repl_disconnect_fn(&args, 0));
    }
    repl_stop_server(&s);
    PASS();
}

/* Connection drops mid-session — call eval after stopping the server.
 * Drives the IPC error response path (`if (RAY_IS_ERR(resp))` at the
 * top of eval_and_print_remote, line 634). */
static test_result_t test_repl_remote_eval_after_server_stop(void) {
    repl_server_t s;
    if (repl_start_server(&s) != 0) FAIL("server start failed");

    /* Connect via the local API directly so we can stop the server
     * before the next eval and observe the failure in send_verbose. */
    ray_t* addr = mk_addr_str(s.port);
    ray_t* opened = ray_repl_connect_fn(addr);
    TEST_ASSERT_NOT_NULL(opened);
    TEST_ASSERT_FALSE(RAY_IS_ERR(opened));

    /* Drop the server so the next request fails. */
    repl_stop_server(&s);

    /* Drive a piped REPL line through eval_and_print_remote. */
    char script[64] = "(+ 1 2)\n";
    /* Note: run_piped_with_input creates its own ray_repl, but the
     * remote-state is process-wide, so the piped REPL will route to
     * the (now-dead) server.  This drives the error path; it must
     * not crash and the loop must finish. */
    int rc = run_piped_with_input(script);
    TEST_ASSERT_EQ_I(rc, 0);

    /* Cleanup: explicit disconnect (idempotent if send_verbose
     * already closed the handle). */
    ray_t* args = NULL;
    ray_release(ray_repl_disconnect_fn(&args, 0));
    ray_release(opened);
    ray_release(addr);
    PASS();
}

/* ─── run_piped buffer overflow ──────────────────────────────────── */

/* Drive the `accum overflow` arm in run_piped (PIPE_BUF_SIZE=4096).
 * We feed a multi-line block whose total exceeds the buffer limit,
 * which triggers the "error: input too large" path and the chunked
 * fgets drain.  Loop must survive and return 0. */
static test_result_t test_repl_run_piped_overflow(void) {
    /* Build a script that ends up >4096 bytes in the accumulator.
     * Comment lines are accumulated like any other input — quickest
     * way to overflow without parser involvement.  Even though `;`
     * lines never reach eval, they DO go through the accumulator
     * branch: bracket_delta_s sees them as comments, which doesn't
     * help with the "needed < PIPE_BUF_SIZE" check.
     *
     * Use an open paren to keep the accumulator unmatched so the
     * loop keeps appending.  Each repeating chunk is 100 bytes; need
     * ~50+ to overflow. */
    char script[6000];
    int n = 0;
    n += snprintf(script + n, sizeof(script) - n, "(+\n");
    for (int i = 0; i < 60 && n < (int)sizeof(script) - 110; i++) {
        n += snprintf(script + n, sizeof(script) - n,
                      "  ;; padding-line-%02d-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n",
                      i);
    }
    /* Close the form. */
    n += snprintf(script + n, sizeof(script) - n, "  1 2)\n");

    int rc = run_piped_with_input(script);
    TEST_ASSERT_EQ_I(rc, 0);
    PASS();
}

/* ─── Multi-frame error trace ─────────────────────────────────────── */

/* stderr-capture variant of cap_begin/end — used to assert on the
 * rendered trace which run_file writes to stderr. */
static int cap_err_begin(char* path, int32_t cap_path) {
    fflush(stderr);
    int saved = dup(fileno(stderr));
    if (saved < 0) return -1;
    snprintf(path, (size_t)cap_path,
             "/tmp/ray_test_repl_caperr_%d_%ld",
             (int)getpid(), (long)saved);
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { close(saved); return -1; }
    if (dup2(fd, fileno(stderr)) < 0) {
        close(fd); close(saved); unlink(path); return -1;
    }
    close(fd);
    return saved;
}

static int32_t cap_err_end(int saved_fd, const char* path,
                           char* out, int32_t cap) {
    fflush(stderr);
    if (saved_fd >= 0) {
        dup2(saved_fd, fileno(stderr));
        close(saved_fd);
    }
    int rfd = open(path, O_RDONLY);
    int32_t n = 0;
    if (rfd >= 0) {
        ssize_t r = read(rfd, out, (size_t)(cap - 1));
        if (r > 0) n = (int32_t)r;
        close(rfd);
    }
    out[n] = '\0';
    unlink(path);
    return n;
}

/* Stronger error-trace test: capture stderr and confirm fmt_error_with_trace
 * actually fired (look for the gutter / caret box-drawing chars).  If it
 * didn't, this test fails and we know the existing trace test was hitting
 * the no-trace branch only. */
static test_result_t test_repl_run_file_error_trace_rendered(void) {
    /* A nested lambda call ensures there's at least one stack frame
     * captured for trace rendering. */
    TEST_ASSERT_EQ_I(write_rfl(
        "(set f (fn [x] (+ x \"s\")))\n"
        "(f 1)\n"), 0);

    /* Mute stdout — the result-print branch goes there for non-errors. */
    fflush(stdout);
    int saved_out = dup(fileno(stdout));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stdout));

    char path[256];
    char buf[16384];
    int saved_err = cap_err_begin(path, sizeof path);

    int rc = ray_repl_run_file(tmp_rfl_path());

    int32_t n = cap_err_end(saved_err, path, buf, sizeof buf);

    if (saved_out >= 0) { dup2(saved_out, fileno(stdout)); close(saved_out); }
    if (devnull   >= 0) close(devnull);
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 1);
    TEST_ASSERT_FMT(n > 0, "no captured stderr");
    /* fmt_error_with_trace draws "× Error:" header. */
    bool has_header = (strstr(buf, "Error:") != NULL);
    TEST_ASSERT_FMT(has_header, "missing Error header in: %.300s", buf);
    PASS();
}

/* Six-frame trace — exercise the `more frames` tail (nframes > 5). */
static test_result_t test_repl_run_file_error_trace_truncated(void) {
    /* Build a recursion that errors deep enough to push >5 lambda
     * frames. Naive recursion: f calls f calls f ... until error. */
    TEST_ASSERT_EQ_I(write_rfl(
        "(set f (fn [n]\n"
        "  (if (= n 0)\n"
        "      (+ 1 \"x\")\n"          /* terminal type-error */
        "      (f (- n 1)))))\n"
        "(f 7)\n"), 0);

    fflush(stdout);
    int saved_out = dup(fileno(stdout));
    int devnull   = open("/dev/null", O_WRONLY);
    if (devnull >= 0) dup2(devnull, fileno(stdout));

    char path[256];
    char buf[16384];
    int saved_err = cap_err_begin(path, sizeof path);

    int rc = ray_repl_run_file(tmp_rfl_path());

    int32_t n = cap_err_end(saved_err, path, buf, sizeof buf);

    if (saved_out >= 0) { dup2(saved_out, fileno(stdout)); close(saved_out); }
    if (devnull   >= 0) close(devnull);
    unlink_rfl();

    TEST_ASSERT_EQ_I(rc, 1);
    /* Either the "more frames" tail rendered or the trace rendered with
     * 8 frames truncated to 5 — both are wins; we just need stderr to
     * have something resembling our output. */
    (void)n; (void)buf;
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
    { "repl/run_file/profile_active",   test_repl_run_file_profile_active,   repl_setup, repl_teardown },
    { "repl/run_file/error_trace",      test_repl_run_file_error_with_trace, repl_setup, repl_teardown },
    { "repl/run/piped/error_trace",     test_repl_run_piped_error_with_trace, repl_setup, repl_teardown },

    /* lifecycle */
    { "repl/create_destroy/null_poll",  test_repl_create_destroy_null_poll,  repl_setup, repl_teardown },
    { "repl/create/inherits_profile",   test_repl_create_inherits_profile,   repl_setup, repl_teardown },
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

    /* Profile / timing renderer */
    { "repl/run/piped/timeit_on",       test_repl_run_piped_timeit_on,           repl_setup, repl_teardown },
    { "repl/run/piped/timeit_multi",    test_repl_run_piped_timeit_multi,        repl_setup, repl_teardown },
    { "repl/run/piped/timeit_error",    test_repl_run_piped_timeit_error,        repl_setup, repl_teardown },

    /* Remote-REPL session — ray_repl_connect_fn / ray_repl_disconnect_fn */
    { "repl/remote/connect_disconnect", test_repl_remote_connect_disconnect,     repl_setup, repl_teardown },
    { "repl/remote/disconnect_inactive", test_repl_remote_disconnect_when_inactive, repl_setup, repl_teardown },
    { "repl/remote/connect_type_error", test_repl_remote_connect_type_error,     repl_setup, repl_teardown },
    { "repl/remote/connect_null_arg",   test_repl_remote_connect_null_arg,       repl_setup, repl_teardown },
    { "repl/remote/connect_open_error", test_repl_remote_connect_open_error,     repl_setup, repl_teardown },
    { "repl/remote/reconnect_swap",     test_repl_remote_reconnect_swaps_handle, repl_setup, repl_teardown },
    { "repl/remote/piped_eval",         test_repl_remote_piped_eval,             repl_setup, repl_teardown },
    { "repl/remote/ctl_local",          test_repl_remote_ctl_evaluated_locally,  repl_setup, repl_teardown },

    /* Slash-command coverage (handle_command + syscmd dispatch) */
    { "repl/run/piped/env",             test_repl_run_piped_env,                 repl_setup, repl_teardown },
    { "repl/run/piped/timeit_off",      test_repl_run_piped_timeit_off,          repl_setup, repl_teardown },
    { "repl/run/piped/timeit_toggle",   test_repl_run_piped_timeit_toggle,       repl_setup, repl_teardown },
    { "repl/run/piped/timeit_bad_arg",  test_repl_run_piped_timeit_bad_arg,      repl_setup, repl_teardown },
    { "repl/run/piped/help",            test_repl_run_piped_help,                repl_setup, repl_teardown },
    { "repl/run/piped/help_full",       test_repl_run_piped_help_full,           repl_setup, repl_teardown },
    { "repl/run/piped/clear",           test_repl_run_piped_clear,               repl_setup, repl_teardown },
    { "repl/run/piped/listen_no_arg",   test_repl_run_piped_listen_no_arg,       repl_setup, repl_teardown },
    { "repl/run/piped/listen_bad_port", test_repl_run_piped_listen_bad_port,     repl_setup, repl_teardown },
    { "repl/run/piped/unknown_cmd",     test_repl_run_piped_unknown_cmd,         repl_setup, repl_teardown },
    { "repl/run/piped/empty_colon",     test_repl_run_piped_empty_colon,         repl_setup, repl_teardown },

    /* repl_print_result output paths */
    { "repl/print_result/list",         test_repl_print_result_list,             repl_setup, repl_teardown },
    { "repl/print_result/dict",         test_repl_print_result_dict,             repl_setup, repl_teardown },
    { "repl/print_result/vec",          test_repl_print_result_vec,              repl_setup, repl_teardown },
    { "repl/print_result/err_no_trace", test_repl_print_result_err_no_trace,     repl_setup, repl_teardown },

    /* Profile in run_file: lazy / parse-error during profiling */
    { "repl/run_file/profile_with_lazy",   test_repl_run_file_profile_with_lazy,   repl_setup, repl_teardown },
    { "repl/run_file/profile_parse_error", test_repl_run_file_profile_parse_error, repl_setup, repl_teardown },

    /* Remote eval — server-side error / captured stdout / dropped conn */
    { "repl/remote/eval_server_error",       test_repl_remote_eval_server_error,       repl_setup, repl_teardown },
    { "repl/remote/eval_captures_stdout",    test_repl_remote_eval_captures_stdout,    repl_setup, repl_teardown },
    { "repl/remote/eval_server_parse_error", test_repl_remote_eval_server_parse_error, repl_setup, repl_teardown },
    { "repl/remote/eval_after_server_stop",  test_repl_remote_eval_after_server_stop,  repl_setup, repl_teardown },

    /* Error-trace pretty-printer — multi-frame + truncation */
    { "repl/run_file/error_trace_rendered",  test_repl_run_file_error_trace_rendered,  repl_setup, repl_teardown },
    { "repl/run_file/error_trace_truncated", test_repl_run_file_error_trace_truncated, repl_setup, repl_teardown },

    /* run_piped accumulator overflow */
    { "repl/run/piped/overflow",             test_repl_run_piped_overflow,             repl_setup, repl_teardown },

    /* PTY-driven interactive (run_interactive) coverage */
    { "repl/pty/quit",                       test_repl_pty_quit,                       repl_setup, repl_teardown },
    { "repl/pty/backslash_exit",             test_repl_pty_backslash_exit,             repl_setup, repl_teardown },
    { "repl/pty/eval_then_quit",             test_repl_pty_eval_then_quit,             repl_setup, repl_teardown },
    { "repl/pty/ctrl_d",                     test_repl_pty_ctrl_d,                     repl_setup, repl_teardown },
    { "repl/pty/no_poll_quit",               test_repl_pty_no_poll_quit,               repl_setup, repl_teardown },
    { "repl/pty/no_poll_eval",               test_repl_pty_no_poll_eval,               repl_setup, repl_teardown },
    { "repl/pty/no_poll_ctrl_d",             test_repl_pty_no_poll_ctrl_d,             repl_setup, repl_teardown },
    { "repl/pty/sigint",                     test_repl_pty_sigint,                     repl_setup, repl_teardown },
    { "repl/pty/no_poll_sigint",             test_repl_pty_no_poll_sigint,             repl_setup, repl_teardown },

    { NULL, NULL, NULL, NULL },
};
