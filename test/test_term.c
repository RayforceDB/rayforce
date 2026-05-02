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
 * Coverage tests for src/app/term.c — the Rayforce REPL's terminal layer.
 *
 * Most of term.c is interactive (raw-mode reads, signal install, the
 * keystroke loop) and not suitable for direct unit tests.  The pieces that
 * ARE testable from a non-TTY context:
 *
 *   1. ray_hist_create / _destroy / _add / _prev / _next / _search /
 *      _save / _load — the history buffer + on-disk persistence.
 *   2. ray_term_visual_width — visual-column accounting that strips ANSI
 *      escape sequences and counts UTF-8 code points.
 *   3. ray_term_find_matching_paren — bracket matching with string-literal
 *      handling.
 *   4. ray_term_count_unmatched — multi-line continuation predicate.
 *   5. ray_term_goto_position + the bare-printf cursor / line / hide-show
 *      helpers — pure escape-code emitters; we redirect stdout to a
 *      tempfile and assert on the captured bytes.
 *   6. ray_term_collect_completions — pulls names from the global env
 *      (under a real runtime), keywords, history words, and table
 *      column names.
 *
 * Skipped (genuinely interactive / TTY-dependent and not worth a fragile
 * harness):
 *   - ray_term_create / _destroy (calls tcgetattr/tcsetattr on stdin)
 *   - ray_term_getc, ray_term_read, ray_term_feed, ray_term_begin
 *   - ray_term_prompt, ray_term_redraw, search-mode redraw
 *   - signal handlers, atexit handler, eval_begin/end (termios mutation)
 *
 * History rules:
 *   - All allocations use the project allocator (ray_alloc / ray_free
 *     indirectly via ray_hist_*).  No libc malloc anywhere in this file.
 *   - Test-temp files live under /tmp with a per-pid unique name and are
 *     unlinked after the test (or on tearDown).
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "table/sym.h"
#include "lang/env.h"
#include "app/term.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

/* Forward declare the runtime API used by completion tests. */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

/* ─── Setup / teardown ─────────────────────────────────────────────── */

static void heap_setup(void)    { ray_heap_init(); (void)ray_sym_init(); }
static void heap_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); }

static void runtime_setup(void)    { ray_runtime_create(0, NULL); }
static void runtime_teardown(void) { ray_runtime_destroy(__RUNTIME); }

/* ─── Stdout-capture plumbing ──────────────────────────────────────── */

/* Redirect stdout to a freshly-truncated temp file under /tmp.  The fd
 * returned must be passed to capture_end() — that closes the temp file,
 * restores the original stdout, and copies up to cap-1 bytes of captured
 * output into out.  Returns -1 on setup failure (test should bail). */
static int capture_begin(char* tmppath, int32_t cap_path) {
    fflush(stdout);
    int saved = dup(fileno(stdout));
    if (saved < 0) return -1;
    snprintf(tmppath, (size_t)cap_path,
             "/tmp/ray_term_test_cap_%d_%ld",
             (int)getpid(), (long)saved);
    int fd = open(tmppath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { close(saved); return -1; }
    if (dup2(fd, fileno(stdout)) < 0) {
        close(fd); close(saved); unlink(tmppath); return -1;
    }
    close(fd);
    return saved;
}

static int32_t capture_end(int saved_fd, const char* tmppath,
                            char* out, int32_t cap) {
    fflush(stdout);
    if (saved_fd >= 0) {
        dup2(saved_fd, fileno(stdout));
        close(saved_fd);
    }
    int rfd = open(tmppath, O_RDONLY);
    int32_t n = 0;
    if (rfd >= 0) {
        ssize_t r = read(rfd, out, (size_t)(cap - 1));
        if (r > 0) n = (int32_t)r;
        close(rfd);
    }
    out[n] = '\0';
    unlink(tmppath);
    return n;
}

/* ─── ray_term_get_size ────────────────────────────────────────────── */

/* Calls ioctl(TIOCGWINSZ) on stdout.  Under a non-TTY runner stdout
 * isn't a terminal, so the call fails and we get the documented fallback
 * (80x24).  When run from an interactive shell ioctl succeeds and we
 * just verify both fields are positive.  The point is to exercise the
 * function and the fallback branch — not to assert exact dimensions. */
static test_result_t test_term_get_size(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;

    ray_term_get_size(t);
    TEST_ASSERT_FMT(t->term_width  > 0, "width=%d not positive",  t->term_width);
    TEST_ASSERT_FMT(t->term_height > 0, "height=%d not positive", t->term_height);

    ray_free(block);
    PASS();
}

/* ─── Tests for the cursor/line escape printers ────────────────────── */

static test_result_t test_term_cursor_move_basics(void) {
    char path[256];
    char buf[256];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) FAIL("capture setup failed");

    /* All four directions with positive n */
    ray_cursor_move_left(3);
    ray_cursor_move_right(7);
    ray_cursor_move_up(1);
    ray_cursor_move_down(5);
    /* n <= 0 is a no-op for left/right/up/down */
    ray_cursor_move_left(0);
    ray_cursor_move_right(-1);
    ray_cursor_move_up(0);
    ray_cursor_move_down(-2);
    /* Move-start emits a CR */
    ray_cursor_move_start();

    int32_t n = capture_end(saved, path, buf, sizeof buf);
    TEST_ASSERT_FMT(n > 0, "no bytes captured");
    TEST_ASSERT_FMT(strstr(buf, "\033[3D") != NULL, "missing CSI 3D");
    TEST_ASSERT_FMT(strstr(buf, "\033[7C") != NULL, "missing CSI 7C");
    TEST_ASSERT_FMT(strstr(buf, "\033[1A") != NULL, "missing CSI 1A");
    TEST_ASSERT_FMT(strstr(buf, "\033[5B") != NULL, "missing CSI 5B");
    TEST_ASSERT_FMT(strchr(buf, '\r') != NULL, "missing CR from move_start");
    /* No-op variants must not emit anything for those n values */
    TEST_ASSERT_FMT(strstr(buf, "\033[0D") == NULL, "left(0) emitted CSI");
    TEST_ASSERT_FMT(strstr(buf, "\033[-1C") == NULL, "right(-1) emitted CSI");
    PASS();
}

static test_result_t test_term_line_clear_and_visibility(void) {
    char path[256];
    char buf[256];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) FAIL("capture setup failed");

    ray_line_clear();        /* "\r\033[K" */
    ray_line_clear_below();  /* "\033[J" */
    ray_cursor_hide();       /* "\033[?25l" */
    ray_cursor_show();       /* "\033[?25h" */

    int32_t n = capture_end(saved, path, buf, sizeof buf);
    TEST_ASSERT_FMT(n > 0, "no bytes captured");
    TEST_ASSERT_FMT(strstr(buf, "\033[K")  != NULL, "missing line clear");
    TEST_ASSERT_FMT(strstr(buf, "\033[J")  != NULL, "missing clear below");
    TEST_ASSERT_FMT(strstr(buf, "\033[?25l") != NULL, "missing cursor hide");
    TEST_ASSERT_FMT(strstr(buf, "\033[?25h") != NULL, "missing cursor show");
    PASS();
}

/* ─── ray_term_visual_width ───────────────────────────────────────── */

static test_result_t test_term_visual_width_ascii(void) {
    TEST_ASSERT_EQ_I(ray_term_visual_width("",      0), 0);
    TEST_ASSERT_EQ_I(ray_term_visual_width("hello", 5), 5);
    TEST_ASSERT_EQ_I(ray_term_visual_width("a b",   3), 3);
    PASS();
}

static test_result_t test_term_visual_width_strips_escapes(void) {
    /* ANSI sequences contribute zero width, payload counts normally */
    const char* s = "\033[31mred\033[0m!";
    int32_t len = (int32_t)strlen(s);
    TEST_ASSERT_EQ_I(ray_term_visual_width(s, len), 4);  /* "red!" */

    /* Multi-parameter SGR */
    const char* s2 = "\033[1;38;5;39mblue\033[0m";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s2, (int32_t)strlen(s2)), 4);
    PASS();
}

static test_result_t test_term_visual_width_utf8(void) {
    /* "‣" U+2023 is 3 bytes UTF-8 (E2 80 A3), 1 column wide */
    const char* s = "\xe2\x80\xa3";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s, 3), 1);

    /* Mixed ASCII + 2-byte UTF-8 (e.g. é = C3 A9) */
    const char* s2 = "ca\xc3\xa9";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s2, (int32_t)strlen(s2)), 3);

    /* 4-byte sequence (e.g. emoji) — counted as 2 columns */
    const char* s3 = "\xf0\x9f\x98\x80";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s3, 4), 2);
    PASS();
}

/* Bare ESC followed by a non-CSI byte — exits escape state immediately.
 * Without the dedicated state-1 handling we'd over-count the bytes. */
static test_result_t test_term_visual_width_bare_escape(void) {
    /* "\033X" — the X is not '[' or 'O', so escape ends after consuming
     * X.  Visual width = 0 (both bytes were part of the escape). */
    const char* s = "\033X";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s, 2), 0);
    /* "a\033Xb" — 'a' counts, "\033X" doesn't, 'b' counts. */
    const char* s2 = "a\033Xb";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s2, 4), 2);
    /* SS3 sequence "\033Of" — 'O' is the introducer, 'f' is final byte. */
    const char* s3 = "\033Ofz";
    TEST_ASSERT_EQ_I(ray_term_visual_width(s3, 4), 1); /* only 'z' */
    PASS();
}

/* ─── ray_term_find_matching_paren ────────────────────────────────── */

static test_result_t test_term_find_matching_paren_simple(void) {
    const char* s = "(abc)";
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 5, 0), 4);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 5, 4), 0);

    const char* s2 = "[ ( ) ]";
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s2, 7, 0), 6);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s2, 7, 6), 0);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s2, 7, 2), 4);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s2, 7, 4), 2);
    PASS();
}

static test_result_t test_term_find_matching_paren_nested(void) {
    const char* s = "((x)(y))";
    /* Outer */
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 8, 0), 7);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 8, 7), 0);
    /* Inner pair (x) at 1..3 */
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 8, 1), 3);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 8, 3), 1);
    /* Inner pair (y) at 4..6 */
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 8, 4), 6);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 8, 6), 4);
    PASS();
}

static test_result_t test_term_find_matching_paren_unmatched(void) {
    /* No match -> -1.  Cursor on a non-bracket -> -1. */
    const char* s = "(abc";
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 4, 0), -1);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 4, 2), -1); /* 'b' */
    /* Out-of-range cursor */
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 4, 4), -1);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 4, -1), -1);
    PASS();
}

static test_result_t test_term_find_matching_paren_braces_brackets(void) {
    /* {} */
    const char* s1 = "{a{b}c}";
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s1, 7, 0), 6);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s1, 7, 6), 0);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s1, 7, 2), 4);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s1, 7, 4), 2);
    /* [] */
    const char* s2 = "[1[2]3]";
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s2, 7, 0), 6);
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s2, 7, 6), 0);
    /* Mismatched brace types: the outer counts depth on opens of one kind,
     * but matching of '{' with '}' is local; an interleaved '(' inside
     * doesn't close the brace. */
    const char* s3 = "{ ( ) }";
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s3, 7, 0), 6);
    PASS();
}

static test_result_t test_term_find_matching_paren_in_string(void) {
    /* Bracket inside a "" literal should not be considered a real bracket. */
    const char* s = "(\"(\")";  /* 5 chars: ( " ( " ) */
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 5, 0), 4);
    /* Cursor on the bracket inside the string — flagged as in-string,
     * returns -1. */
    TEST_ASSERT_EQ_I(ray_term_find_matching_paren(s, 5, 2), -1);
    PASS();
}

/* ─── History: create / add / nav / destroy ───────────────────────── */

static test_result_t test_hist_create_destroy(void) {
    ray_hist_t h;
    ray_hist_create(&h);
    TEST_ASSERT_NOT_NULL(h.entries);
    TEST_ASSERT_EQ_I(h.count, 0);
    TEST_ASSERT_EQ_I(h.capacity, HIST_DEFAULT_CAP);
    TEST_ASSERT_EQ_I(h.index, 0);
    TEST_ASSERT_EQ_I(h.curr_saved, 0);
    ray_hist_destroy(&h);
    TEST_ASSERT_NULL(h.entries);
    TEST_ASSERT_EQ_I(h.count, 0);
    /* Double-destroy must be a no-op */
    ray_hist_destroy(&h);
    PASS();
}

static test_result_t test_hist_add_basic(void) {
    ray_hist_t h;
    ray_hist_create(&h);

    ray_hist_add(&h, "alpha", 5);
    ray_hist_add(&h, "beta",  4);
    ray_hist_add(&h, "gamma", 5);
    TEST_ASSERT_EQ_I(h.count, 3);
    TEST_ASSERT_STR_EQ(h.entries[0], "alpha");
    TEST_ASSERT_STR_EQ(h.entries[1], "beta");
    TEST_ASSERT_STR_EQ(h.entries[2], "gamma");
    /* Index points past last */
    TEST_ASSERT_EQ_I(h.index, 3);

    /* len <= 0 must be ignored */
    ray_hist_add(&h, "x", 0);
    ray_hist_add(&h, "x", -1);
    TEST_ASSERT_EQ_I(h.count, 3);

    /* Consecutive duplicate must NOT be appended (still resets index) */
    ray_hist_add(&h, "gamma", 5);
    TEST_ASSERT_EQ_I(h.count, 3);
    TEST_ASSERT_EQ_I(h.index, 3);

    ray_hist_destroy(&h);
    PASS();
}

static test_result_t test_hist_add_grows(void) {
    ray_hist_t h;
    ray_hist_create(&h);
    int32_t initial_cap = h.capacity;

    /* Push past initial capacity to exercise the grow path */
    char buf[16];
    for (int i = 0; i < initial_cap + 4; i++) {
        snprintf(buf, sizeof buf, "e%05d", i);
        ray_hist_add(&h, buf, (int32_t)strlen(buf));
    }
    TEST_ASSERT_EQ_I(h.count, initial_cap + 4);
    TEST_ASSERT_TRUE(h.capacity > initial_cap);
    /* Verify first and last entries still intact post-grow (memcpy path) */
    TEST_ASSERT_STR_EQ(h.entries[0], "e00000");
    snprintf(buf, sizeof buf, "e%05d", initial_cap + 3);
    TEST_ASSERT_STR_EQ(h.entries[h.count - 1], buf);
    ray_hist_destroy(&h);
    PASS();
}

static test_result_t test_hist_prev_next(void) {
    ray_hist_t h;
    ray_hist_create(&h);
    ray_hist_add(&h, "one",   3);
    ray_hist_add(&h, "two",   3);
    ray_hist_add(&h, "three", 5);

    char buf[64];
    /* Pre-set buf to simulate "user's current input". */
    strcpy(buf, "draft");

    /* prev moves index from 3 -> 2 -> 1 -> 0, returns matching len */
    int32_t len = ray_hist_prev(&h, buf, 5);
    TEST_ASSERT_EQ_I(len, 5);
    TEST_ASSERT_STR_EQ(buf, "three");

    len = ray_hist_prev(&h, buf, len);
    TEST_ASSERT_EQ_I(len, 3);
    TEST_ASSERT_STR_EQ(buf, "two");

    len = ray_hist_prev(&h, buf, len);
    TEST_ASSERT_EQ_I(len, 3);
    TEST_ASSERT_STR_EQ(buf, "one");

    /* Going past the start returns -1 */
    TEST_ASSERT_EQ_I(ray_hist_prev(&h, buf, len), -1);

    /* next: 0 -> 1 -> 2 -> 3 (restore saved current) */
    len = ray_hist_next(&h, buf);
    TEST_ASSERT_EQ_I(len, 3);
    TEST_ASSERT_STR_EQ(buf, "two");

    len = ray_hist_next(&h, buf);
    TEST_ASSERT_EQ_I(len, 5);
    TEST_ASSERT_STR_EQ(buf, "three");

    /* Past last entry: restore saved "draft" */
    len = ray_hist_next(&h, buf);
    TEST_ASSERT_EQ_I(len, 5);
    TEST_ASSERT_STR_EQ(buf, "draft");

    /* Further next returns -1 (past end, nothing saved) */
    TEST_ASSERT_EQ_I(ray_hist_next(&h, buf), -1);

    ray_hist_destroy(&h);
    PASS();
}

static test_result_t test_hist_prev_empty(void) {
    ray_hist_t h;
    ray_hist_create(&h);
    char buf[8] = "x";
    TEST_ASSERT_EQ_I(ray_hist_prev(&h, buf, 1), -1);
    TEST_ASSERT_EQ_I(ray_hist_next(&h, buf), -1);
    ray_hist_destroy(&h);
    PASS();
}

/* ─── History search ──────────────────────────────────────────────── */

static test_result_t test_hist_search(void) {
    ray_hist_t h;
    ray_hist_create(&h);
    ray_hist_add(&h, "select * from foo",  17);
    ray_hist_add(&h, "select count from bar", 21);
    ray_hist_add(&h, "drop table baz",     14);

    /* Search backward from end for "select" — finds index 1 first */
    int32_t idx = ray_hist_search(&h, "select", 6, h.count - 1);
    TEST_ASSERT_EQ_I(idx, 1);

    /* Continue from idx-1 -> finds index 0 */
    idx = ray_hist_search(&h, "select", 6, 0);
    TEST_ASSERT_EQ_I(idx, 0);

    /* No match */
    TEST_ASSERT_EQ_I(ray_hist_search(&h, "xyzzy", 5, h.count - 1), -1);

    /* Empty needle -> -1 */
    TEST_ASSERT_EQ_I(ray_hist_search(&h, "", 0, h.count - 1), -1);

    /* Negative start -> -1 */
    TEST_ASSERT_EQ_I(ray_hist_search(&h, "select", 6, -1), -1);

    /* Start beyond count gets clamped */
    TEST_ASSERT_EQ_I(ray_hist_search(&h, "drop", 4, 999), 2);

    /* Needle longer than entry — skips over short entries cleanly */
    ray_hist_add(&h, "z", 1);
    TEST_ASSERT_EQ_I(ray_hist_search(&h, "longneedle", 10, h.count - 1), -1);

    ray_hist_destroy(&h);
    PASS();
}

/* ─── History save/load round-trip ────────────────────────────────── */

static void make_temp_path(char* out, int32_t cap, const char* tag) {
    snprintf(out, (size_t)cap, "/tmp/ray_term_test_hist_%d_%s",
             (int)getpid(), tag);
    unlink(out);
}

static test_result_t test_hist_save_load_roundtrip(void) {
    char path[256];
    make_temp_path(path, sizeof path, "save_load");

    /* Populate hist1, save */
    ray_hist_t h1;
    ray_hist_create(&h1);
    ray_hist_add(&h1, "first",   5);
    ray_hist_add(&h1, "second",  6);
    ray_hist_add(&h1, "third",   5);
    ray_hist_save(&h1, path);
    ray_hist_destroy(&h1);

    /* Confirm file exists and has nonzero size */
    struct stat st;
    if (stat(path, &st) != 0) { unlink(path); FAIL("save did not create file"); }
    if (st.st_size <= 0)      { unlink(path); FAIL("save produced empty file"); }

    /* Load into fresh hist2, verify */
    ray_hist_t h2;
    ray_hist_create(&h2);
    ray_hist_load(&h2, path);
    int32_t cnt = h2.count;
    int loaded_match =
        cnt == 3
        && strcmp(h2.entries[0], "first")  == 0
        && strcmp(h2.entries[1], "second") == 0
        && strcmp(h2.entries[2], "third")  == 0;
    ray_hist_destroy(&h2);
    unlink(path);
    if (!loaded_match)
        FAILF("round-trip mismatch: count=%d", cnt);
    PASS();
}

static test_result_t test_hist_save_empty_no_file(void) {
    /* Empty hist => save is a no-op, file not created */
    char path[256];
    make_temp_path(path, sizeof path, "empty");

    ray_hist_t h;
    ray_hist_create(&h);
    ray_hist_save(&h, path);
    struct stat st;
    int exists = (stat(path, &st) == 0);
    ray_hist_destroy(&h);
    unlink(path);
    TEST_ASSERT_FMT(!exists, "save with empty hist created a file");
    PASS();
}

static test_result_t test_hist_load_missing_file(void) {
    /* Load from a path that doesn't exist — must be a no-op (and not crash) */
    char path[256];
    snprintf(path, sizeof path, "/tmp/ray_term_test_nope_%d", (int)getpid());
    unlink(path); /* be sure */

    ray_hist_t h;
    ray_hist_create(&h);
    ray_hist_load(&h, path);
    TEST_ASSERT_EQ_I(h.count, 0);
    ray_hist_destroy(&h);
    PASS();
}

static test_result_t test_hist_load_empty_file(void) {
    /* A 0-byte file should also be a no-op */
    char path[256];
    make_temp_path(path, sizeof path, "empty_file");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) FAIL("could not create empty file");
    close(fd);

    ray_hist_t h;
    ray_hist_create(&h);
    ray_hist_load(&h, path);
    int32_t cnt = h.count;
    ray_hist_destroy(&h);
    unlink(path);
    TEST_ASSERT_EQ_I(cnt, 0);
    PASS();
}

/* Verify load skips over consecutive null bytes (empty entries) and that
 * entries written by save can also be loaded verbatim back. */
static test_result_t test_hist_load_skips_empty_records(void) {
    char path[256];
    make_temp_path(path, sizeof path, "skip_empty");

    /* Hand-craft a file: "a\0\0b\0" — load should yield {"a","b"}. */
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) FAIL("temp write failed");
    const char raw[] = { 'a', '\0', '\0', 'b', '\0' };
    ssize_t w = write(fd, raw, sizeof raw);
    (void)w;
    close(fd);

    ray_hist_t h;
    ray_hist_create(&h);
    ray_hist_load(&h, path);
    int matched = h.count == 2
        && strcmp(h.entries[0], "a") == 0
        && strcmp(h.entries[1], "b") == 0;
    ray_hist_destroy(&h);
    unlink(path);
    TEST_ASSERT_FMT(matched, "expected 2 entries 'a','b'");
    PASS();
}

/* ─── Multi-line bracket counting ─────────────────────────────────── */

static test_result_t test_term_count_unmatched_basic(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;

    /* Empty buf => no unmatched */
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 0);

    /* Single open => 1 */
    strcpy(t->buf, "(");
    t->buf_len = 1;
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 1);

    /* Balanced => 0 */
    strcpy(t->buf, "([{}])");
    t->buf_len = 6;
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 0);

    /* Open bracket inside string is ignored */
    strcpy(t->buf, "\"(\"");
    t->buf_len = 3;
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 0);

    /* Comment to end-of-line skips trailing brackets */
    strcpy(t->buf, "(); rest (((");
    t->buf_len = (int32_t)strlen(t->buf);
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 0);

    /* Multiline — unmatched ( on first chunk, not yet closed in buf */
    strcpy(t->multiline_buf, "(foo\n");
    t->multiline_len = 5;
    strcpy(t->buf, "bar");
    t->buf_len = 3;
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 1);

    /* Closed across chunks */
    strcpy(t->multiline_buf, "(foo\n");
    t->multiline_len = 5;
    strcpy(t->buf, "bar)");
    t->buf_len = 4;
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 0);

    /* Escaped quote inside multiline string keeps state correct */
    strcpy(t->multiline_buf, "\"a\\\"b\n");
    t->multiline_len = 6;
    strcpy(t->buf, "c\"(");
    t->buf_len = 3;
    TEST_ASSERT_EQ_I(ray_term_count_unmatched(t), 1);

    ray_free(block);
    PASS();
}

/* ─── ray_term_goto_position ─────────────────────────────────────── */

static test_result_t test_term_goto_position_no_op_zero_width(void) {
    /* term_width <= 0 -> early return, no output */
    char path[256], buf[64];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) FAIL("capture setup failed");

    ray_term_t t;
    memset(&t, 0, sizeof t);
    t.term_width = 0;
    t.prompt_len = 2;
    ray_term_goto_position(&t, 0, 5);

    int32_t n = capture_end(saved, path, buf, sizeof buf);
    TEST_ASSERT_EQ_I(n, 0);
    PASS();
}

static test_result_t test_term_goto_position_emits_movements(void) {
    char path[256], buf[256];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) FAIL("capture setup failed");

    /* Term 80 cols, prompt visual = 2; buf = 10 ASCII chars, no newlines.
     * Move from col 12 (pos=10) -> col 7 (pos=5): same row, 5 cols left. */
    ray_term_t t;
    memset(&t, 0, sizeof t);
    t.term_width = 80;
    t.prompt_len = 2;
    strcpy(t.buf, "abcdefghij");
    t.buf_len = 10;
    ray_term_goto_position(&t, 10, 5);

    int32_t n = capture_end(saved, path, buf, sizeof buf);
    TEST_ASSERT_FMT(n > 0, "no output");
    TEST_ASSERT_FMT(strstr(buf, "\033[5D") != NULL,
                    "missing left-5 movement: %s", buf);
    /* Same-row -> no up/down */
    TEST_ASSERT_FMT(strstr(buf, "[A") == NULL && strstr(buf, "[B") == NULL,
                    "unexpected vertical movement");
    /* Recorded final row = (2+5)/80 = 0 */
    TEST_ASSERT_EQ_I(t.last_cursor_row, 0);
    PASS();
}

static test_result_t test_term_goto_position_wraps_rows(void) {
    char path[256], buf[256];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) FAIL("capture setup failed");

    /* Narrow terminal (10 cols) forces wrapping.  Prompt=2, buf=20 chars.
     * From pos 0 (row 0, col 2) to pos 20 (col total=22 -> row 2, col 2):
     * down 2, right 0. */
    ray_term_t t;
    memset(&t, 0, sizeof t);
    t.term_width = 10;
    t.prompt_len = 2;
    memset(t.buf, 'a', 20);
    t.buf[20] = '\0';
    t.buf_len = 20;
    ray_term_goto_position(&t, 0, 20);

    int32_t n = capture_end(saved, path, buf, sizeof buf);
    TEST_ASSERT_FMT(n > 0, "no output");
    TEST_ASSERT_FMT(strstr(buf, "\033[2B") != NULL,
                    "missing down-2 movement: %s", buf);
    TEST_ASSERT_EQ_I(t.last_cursor_row, 2);
    PASS();
}

static test_result_t test_term_goto_position_up_movement(void) {
    char path[256], buf[256];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) FAIL("capture setup failed");

    /* From pos 20 (row 2) back to pos 0 (row 0): up 2, right 0 (col-aligned)
     * actually col_diff = 2-2 = 0 since prompt_len = 2. */
    ray_term_t t;
    memset(&t, 0, sizeof t);
    t.term_width = 10;
    t.prompt_len = 2;
    memset(t.buf, 'a', 20);
    t.buf[20] = '\0';
    t.buf_len = 20;
    ray_term_goto_position(&t, 20, 0);

    int32_t n = capture_end(saved, path, buf, sizeof buf);
    TEST_ASSERT_FMT(n > 0, "no output");
    TEST_ASSERT_FMT(strstr(buf, "\033[2A") != NULL,
                    "missing up-2 movement: %s", buf);
    TEST_ASSERT_EQ_I(t.last_cursor_row, 0);
    PASS();
}

/* ─── ray_term_redraw (exercises term_highlight_into + goto_position) ── */

/* Redraw is a write-only function — it streams escape sequences and the
 * highlighted buffer to stdout.  We only need it to *not crash* and to
 * emit *something* with the bracket-match colour for a balanced "(x)".
 * The runtime is required for env_has_name lookups inside the highlighter
 * (the keyword green path). */
static test_result_t test_term_redraw_smoke(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    t->term_width = 80;
    t->term_height = 24;
    t->prompt_len = 2;
    t->last_total_rows = 1;
    ray_hist_create(&t->hist);

    /* Buffer with brackets, a string literal, a quoted symbol, a comment,
     * and an operator — exercises every arm of term_highlight_into. */
    const char* src = "(let 'x \"hi\" + 1) ; trailing comment";
    int32_t slen = (int32_t)strlen(src);
    memcpy(t->buf, src, (size_t)slen);
    t->buf_len = slen;
    t->buf_pos = 1;  /* On the '(' so bracket-match path engages */

    char path[256], buf[8192];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) {
        ray_hist_destroy(&t->hist);
        ray_free(block);
        FAIL("capture setup failed");
    }
    ray_term_redraw(t);
    int32_t n = capture_end(saved, path, buf, sizeof buf);

    int output_ok = n > 0;
    /* Bracket-match highlight uses cyan-bg (CSI 46) */
    int saw_match = strstr(buf, "\033[46m") != NULL;
    /* Some color reset must appear */
    int saw_reset = strstr(buf, "\033[0m") != NULL;

    ray_hist_destroy(&t->hist);
    ray_free(block);
    TEST_ASSERT_FMT(output_ok, "no redraw output");
    TEST_ASSERT_FMT(saw_match, "missing bracket-match highlight");
    TEST_ASSERT_FMT(saw_reset, "missing color reset");
    PASS();
}

/* Redraw with cursor BEFORE a closing bracket — the second branch in
 * the cursor-vs-match check (cursor > 0 case) becomes live. */
static test_result_t test_term_redraw_close_bracket(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    t->term_width = 80;
    t->term_height = 24;
    t->prompt_len = 2;
    t->last_total_rows = 1;
    ray_hist_create(&t->hist);

    const char* src = "(abc)";
    int32_t slen = (int32_t)strlen(src);
    memcpy(t->buf, src, (size_t)slen);
    t->buf_len = slen;
    /* Position the cursor right after ')', so the cursor-1 path in
     * ray_term_redraw is what triggers the bracket match. */
    t->buf_pos = slen;

    char path[256], buf[4096];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) {
        ray_hist_destroy(&t->hist);
        ray_free(block);
        FAIL("capture setup failed");
    }
    ray_term_redraw(t);
    int32_t n = capture_end(saved, path, buf, sizeof buf);
    int saw_match = strstr(buf, "\033[46m") != NULL;
    int output_ok = n > 0;

    ray_hist_destroy(&t->hist);
    ray_free(block);
    TEST_ASSERT_FMT(output_ok, "no redraw output");
    TEST_ASSERT_FMT(saw_match, "missing bracket-match for cursor-after-close");
    PASS();
}

/* Redraw with a multiline buffer chooses the continuation prompt branch. */
static test_result_t test_term_redraw_multiline_prompt(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    t->term_width = 80;
    t->term_height = 24;
    t->prompt_len = 2;
    t->last_total_rows = 1;
    ray_hist_create(&t->hist);

    strcpy(t->multiline_buf, "(foo\n");
    t->multiline_len = 5;
    strcpy(t->buf, "bar");
    t->buf_len = 3;
    t->buf_pos = 3;

    char path[256], buf[4096];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) {
        ray_hist_destroy(&t->hist);
        ray_free(block);
        FAIL("capture setup failed");
    }
    ray_term_redraw(t);
    int32_t n = capture_end(saved, path, buf, sizeof buf);
    int output_ok = n > 0;
    /* Continuation prompt uses gray fg \033[90m */
    int saw_cont = strstr(buf, "\033[90m") != NULL;

    ray_hist_destroy(&t->hist);
    ray_free(block);
    TEST_ASSERT_FMT(output_ok, "no redraw output");
    TEST_ASSERT_FMT(saw_cont, "missing continuation prompt color");
    PASS();
}

/* ─── ray_term_collect_completions (needs lang runtime) ────────────── */

static test_result_t test_term_collect_completions_env(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    ray_hist_create(&t->hist);

    /* Prefix "se" should match builtins like "select", "set" registered by
     * the runtime.  We don't hard-code their names — just assert we see at
     * least one match and no crash. */
    ray_term_collect_completions(t, "se", 2);
    TEST_ASSERT_FMT(t->comp_count > 0,
                    "expected at least 1 completion for 'se'; got %d",
                    t->comp_count);
    /* All results must start with the prefix (prefix lookup contract) */
    for (int32_t i = 0; i < t->comp_count; i++) {
        TEST_ASSERT_FMT(t->comp_items[i] != NULL,
                        "comp_items[%d] is NULL", i);
        TEST_ASSERT_FMT(strncmp(t->comp_items[i], "se", 2) == 0,
                        "comp_items[%d]='%s' doesn't start with 'se'",
                        i, t->comp_items[i]);
    }

    /* Empty prefix => 0 results, fast path */
    ray_term_collect_completions(t, "", 0);
    TEST_ASSERT_EQ_I(t->comp_count, 0);

    /* Prefix that nothing matches */
    ray_term_collect_completions(t, "qzqzqzqz", 8);
    TEST_ASSERT_EQ_I(t->comp_count, 0);

    ray_hist_destroy(&t->hist);
    ray_free(block);
    PASS();
}

/* Verify the column-name source: when buf contains "from: NAME", and NAME
 * is bound to a table in the env, completions include matching column
 * names.  Exercises comp_find_from_table + the column iteration path. */
static test_result_t test_term_collect_completions_table_columns(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    ray_hist_create(&t->hist);

    /* Build a tiny table with columns ucol_alpha and ucol_beta, bind it
     * under a unique name `ucol_table` so we don't collide with anything. */
    int64_t n_alpha = ray_sym_intern("ucol_alpha", 10);
    int64_t n_beta  = ray_sym_intern("ucol_beta",  9);
    int64_t v[] = { 1 };
    ray_t* col_a = ray_vec_from_raw(RAY_I64, v, 1);
    ray_t* col_b = ray_vec_from_raw(RAY_I64, v, 1);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_alpha, col_a);
    tbl = ray_table_add_col(tbl, n_beta,  col_b);
    ray_release(col_a);
    ray_release(col_b);
    int64_t n_tbl = ray_sym_intern("ucol_table", 10);
    ray_env_bind(n_tbl, tbl);

    /* Buffer references the table via a `from: ucol_table` query.  The
     * cursor's prefix is "ucol_a" — we should see ucol_alpha. */
    const char* sql = "(select {from: ucol_table} ucol_a";
    int32_t slen = (int32_t)strlen(sql);
    memcpy(t->buf, sql, (size_t)slen);
    t->buf_len = slen;

    ray_term_collect_completions(t, "ucol_a", 6);
    int seen_alpha = 0;
    for (int32_t i = 0; i < t->comp_count; i++) {
        if (strcmp(t->comp_items[i], "ucol_alpha") == 0) seen_alpha = 1;
    }
    /* Be tolerant: the column lookup is one of several sources, and any
     * env-bound user name `ucol_alpha` would also surface — what matters
     * is that the symbol shows up. */
    TEST_ASSERT_FMT(seen_alpha,
                    "expected 'ucol_alpha' in completions; got %d",
                    t->comp_count);

    ray_hist_destroy(&t->hist);
    ray_free(block);
    PASS();
}

static test_result_t test_term_collect_completions_history(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    ray_hist_create(&t->hist);

    /* Add an entry containing a unique word — must surface in completions. */
    ray_hist_add(&t->hist, "(define unicornword 42)", 23);
    ray_term_collect_completions(t, "unicorn", 7);
    int found = 0;
    for (int32_t i = 0; i < t->comp_count; i++) {
        if (strcmp(t->comp_items[i], "unicornword") == 0) { found = 1; break; }
    }
    TEST_ASSERT_FMT(found,
                    "expected 'unicornword' in completions; got %d items",
                    t->comp_count);

    /* Adding a second history hit for the same word should NOT duplicate. */
    ray_hist_add(&t->hist, "use unicornword again", 21);
    ray_term_collect_completions(t, "unicorn", 7);
    int dup_count = 0;
    for (int32_t i = 0; i < t->comp_count; i++) {
        if (strcmp(t->comp_items[i], "unicornword") == 0) dup_count++;
    }
    TEST_ASSERT_EQ_I(dup_count, 1);

    ray_hist_destroy(&t->hist);
    ray_free(block);
    PASS();
}

/* ─── Suite ───────────────────────────────────────────────────────── */

const test_entry_t term_entries[] = {
    /* Terminal size + cursor / line escape printers (no runtime needed) */
    { "term/get_size",                  test_term_get_size,
      heap_setup, heap_teardown },
    { "term/cursor_move_basics",        test_term_cursor_move_basics,
      heap_setup, heap_teardown },
    { "term/line_clear_visibility",     test_term_line_clear_and_visibility,
      heap_setup, heap_teardown },

    /* Visual width */
    { "term/visual_width_ascii",        test_term_visual_width_ascii,
      heap_setup, heap_teardown },
    { "term/visual_width_strips_esc",   test_term_visual_width_strips_escapes,
      heap_setup, heap_teardown },
    { "term/visual_width_utf8",         test_term_visual_width_utf8,
      heap_setup, heap_teardown },
    { "term/visual_width_bare_escape",  test_term_visual_width_bare_escape,
      heap_setup, heap_teardown },

    /* Bracket matching */
    { "term/find_paren_simple",         test_term_find_matching_paren_simple,
      heap_setup, heap_teardown },
    { "term/find_paren_nested",         test_term_find_matching_paren_nested,
      heap_setup, heap_teardown },
    { "term/find_paren_unmatched",      test_term_find_matching_paren_unmatched,
      heap_setup, heap_teardown },
    { "term/find_paren_braces",         test_term_find_matching_paren_braces_brackets,
      heap_setup, heap_teardown },
    { "term/find_paren_in_string",      test_term_find_matching_paren_in_string,
      heap_setup, heap_teardown },

    /* History core */
    { "term/hist_create_destroy",       test_hist_create_destroy,
      heap_setup, heap_teardown },
    { "term/hist_add_basic",            test_hist_add_basic,
      heap_setup, heap_teardown },
    { "term/hist_add_grows",            test_hist_add_grows,
      heap_setup, heap_teardown },
    { "term/hist_prev_next",            test_hist_prev_next,
      heap_setup, heap_teardown },
    { "term/hist_prev_empty",           test_hist_prev_empty,
      heap_setup, heap_teardown },

    /* History search + persistence */
    { "term/hist_search",               test_hist_search,
      heap_setup, heap_teardown },
    { "term/hist_save_load_roundtrip",  test_hist_save_load_roundtrip,
      heap_setup, heap_teardown },
    { "term/hist_save_empty_no_file",   test_hist_save_empty_no_file,
      heap_setup, heap_teardown },
    { "term/hist_load_missing",         test_hist_load_missing_file,
      heap_setup, heap_teardown },
    { "term/hist_load_empty",           test_hist_load_empty_file,
      heap_setup, heap_teardown },
    { "term/hist_load_skip_empty_recs", test_hist_load_skips_empty_records,
      heap_setup, heap_teardown },

    /* Multi-line bracket counting */
    { "term/count_unmatched",           test_term_count_unmatched_basic,
      heap_setup, heap_teardown },

    /* Cursor positioning math */
    { "term/goto_pos_zero_width",       test_term_goto_position_no_op_zero_width,
      heap_setup, heap_teardown },
    { "term/goto_pos_movements",        test_term_goto_position_emits_movements,
      heap_setup, heap_teardown },
    { "term/goto_pos_wraps",            test_term_goto_position_wraps_rows,
      heap_setup, heap_teardown },
    { "term/goto_pos_up",               test_term_goto_position_up_movement,
      heap_setup, heap_teardown },

    /* Redraw + highlight (needs runtime for keyword highlighting + comp_count) */
    { "term/redraw_smoke",              test_term_redraw_smoke,
      runtime_setup, runtime_teardown },
    { "term/redraw_close_bracket",      test_term_redraw_close_bracket,
      runtime_setup, runtime_teardown },
    { "term/redraw_multiline_prompt",   test_term_redraw_multiline_prompt,
      runtime_setup, runtime_teardown },

    /* Completions (need the full runtime — env, sym, keywords, table) */
    { "term/completions_env",           test_term_collect_completions_env,
      runtime_setup, runtime_teardown },
    { "term/completions_table_cols",    test_term_collect_completions_table_columns,
      runtime_setup, runtime_teardown },
    { "term/completions_history",       test_term_collect_completions_history,
      runtime_setup, runtime_teardown },

    { NULL, NULL, NULL, NULL },
};
