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

/* ─── Prompt prefix + prompts ─────────────────────────────────────── */

/* set_prompt_prefix with a normal short string: stores ANSI-wrapped bytes
 * and tracks visual width (unwrapped + 1 for trailing space). */
static test_result_t test_term_set_prompt_prefix_basic(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;

    ray_term_set_prompt_prefix(t, "host:8080");
    TEST_ASSERT_FMT(t->prompt_prefix_len > 0, "prefix_len must be > 0");
    TEST_ASSERT_FMT(t->prompt_prefix_vis == 9 + 1,
                    "expected vis=10, got %d", t->prompt_prefix_vis);
    TEST_ASSERT_FMT(strstr(t->prompt_prefix, "host:8080") != NULL,
                    "prefix bytes missing user string");
    TEST_ASSERT_FMT(strstr(t->prompt_prefix, "\033[33m") != NULL,
                    "prefix missing yellow ANSI start");
    TEST_ASSERT_FMT(strstr(t->prompt_prefix, "\033[0m") != NULL,
                    "prefix missing reset");

    /* Clear via NULL */
    ray_term_set_prompt_prefix(t, NULL);
    TEST_ASSERT_EQ_I(t->prompt_prefix_len, 0);
    TEST_ASSERT_EQ_I(t->prompt_prefix_vis, 0);
    TEST_ASSERT_EQ_I(t->prompt_prefix[0], '\0');

    /* Clear via empty string */
    ray_term_set_prompt_prefix(t, "x");
    TEST_ASSERT_FMT(t->prompt_prefix_len > 0, "set after clear failed");
    ray_term_set_prompt_prefix(t, "");
    TEST_ASSERT_EQ_I(t->prompt_prefix_len, 0);

    /* NULL term must not crash */
    ray_term_set_prompt_prefix(NULL, "abc");

    /* Overflow: snprintf writes ~80 bytes; pass a 200-char string to
     * trip the truncation guard. */
    char big[256];
    memset(big, 'x', 200);
    big[200] = '\0';
    ray_term_set_prompt_prefix(t, big);
    TEST_ASSERT_EQ_I(t->prompt_prefix_len, 0);
    TEST_ASSERT_EQ_I(t->prompt_prefix_vis, 0);

    ray_free(block);
    PASS();
}

static test_result_t test_term_prompt_emits_bytes(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;

    char path[256], cap[512];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) { ray_free(block); FAIL("capture setup failed"); }
    ray_term_prompt(t);
    fflush(stdout);
    int32_t n = capture_end(saved, path, cap, sizeof cap);
    int saw_arrow = strstr(cap, "\xe2\x80\xa3") != NULL; /* ‣ */
    int saw_green = strstr(cap, "\033[32m") != NULL;
    TEST_ASSERT_FMT(n > 0, "no prompt output");
    TEST_ASSERT_FMT(saw_arrow, "missing ‣ arrow in prompt: bytes=%d", n);
    TEST_ASSERT_FMT(saw_green, "missing green ANSI in prompt");
    TEST_ASSERT_EQ_I(t->prompt_len, 2);
    ray_free(block);
    PASS();
}

static test_result_t test_term_prompt_with_prefix(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;

    ray_term_set_prompt_prefix(t, "rmt");
    char path[256], cap[512];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) { ray_free(block); FAIL("capture setup failed"); }
    ray_term_prompt(t);
    fflush(stdout);
    int32_t n = capture_end(saved, path, cap, sizeof cap);
    int saw_prefix = strstr(cap, "rmt") != NULL;
    TEST_ASSERT_FMT(n > 0, "no prompt output");
    TEST_ASSERT_FMT(saw_prefix, "prompt missing prefix bytes");
    /* prompt_len = prefix vis (3+1) + PROMPT_VIS (2) = 6 */
    TEST_ASSERT_EQ_I(t->prompt_len, 6);
    ray_free(block);
    PASS();
}

static test_result_t test_term_continuation_prompt(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;

    char path[256], cap[512];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) { ray_free(block); FAIL("capture setup failed"); }
    ray_term_continuation_prompt(t);
    fflush(stdout);
    int32_t n = capture_end(saved, path, cap, sizeof cap);
    /* Continuation prompt uses gray (\033[90m) and ellipsis (U+2026 = E2 80 A6). */
    int saw_gray = strstr(cap, "\033[90m") != NULL;
    int saw_ellip = strstr(cap, "\xe2\x80\xa6") != NULL;
    TEST_ASSERT_FMT(n > 0, "no cont-prompt output");
    TEST_ASSERT_FMT(saw_gray, "missing gray ANSI in continuation prompt");
    TEST_ASSERT_FMT(saw_ellip, "missing … in continuation prompt");
    TEST_ASSERT_EQ_I(t->prompt_len, 2);

    /* With prefix: prompt_len = prefix_vis + 2 */
    ray_term_set_prompt_prefix(t, "ab");
    saved = capture_begin(path, sizeof path);
    if (saved < 0) { ray_free(block); FAIL("capture setup failed"); }
    ray_term_continuation_prompt(t);
    fflush(stdout);
    n = capture_end(saved, path, cap, sizeof cap);
    TEST_ASSERT_FMT(n > 0, "no cont-prompt output (with prefix)");
    TEST_ASSERT_EQ_I(t->prompt_len, 5); /* 2 for "ab" + 1 space + 2 for cont */

    ray_free(block);
    PASS();
}

/* ─── Interrupt flag ──────────────────────────────────────────────── */

static test_result_t test_term_interrupt_flag(void) {
    ray_term_clear_interrupt();
    TEST_ASSERT_EQ_I(ray_term_interrupted(), 0);
    /* Setting it via private state isn't exposed, but clear+query is. */
    ray_term_clear_interrupt();
    TEST_ASSERT_EQ_I(ray_term_interrupted(), 0);
    PASS();
}

/* ─── ray_term_begin ──────────────────────────────────────────────── */

static test_result_t test_term_begin_resets_state(void) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) FAIL("ray_alloc failed");
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    /* Pre-set fields that begin() must reset */
    strcpy(t->buf, "garbage");
    t->buf_len = 7;
    t->buf_pos = 7;
    t->multiline_len = 5;
    t->last_total_rows = 7;
    t->esc_state = 3;
    t->esc_buf_len = 9;

    char path[256], cap[512];
    int saved = capture_begin(path, sizeof path);
    if (saved < 0) { ray_free(block); FAIL("capture setup failed"); }
    ray_term_begin(t);
    fflush(stdout);
    (void)capture_end(saved, path, cap, sizeof cap);

    TEST_ASSERT_EQ_I(t->buf_len, 0);
    TEST_ASSERT_EQ_I(t->buf_pos, 0);
    TEST_ASSERT_EQ_I(t->multiline_len, 0);
    TEST_ASSERT_EQ_I(t->last_total_rows, 1);
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->esc_buf_len, 0);
    ray_free(block);
    PASS();
}

/* ─── ray_term_feed driver: helpers ───────────────────────────────── */

/* Allocate a fully-zeroed ray_term_t with a sane terminal width and a
 * fresh history (so the keystroke driver has somewhere to push lines).
 * Returns the heap block; caller frees via term_block_free(). */
static ray_t* term_block_new(ray_term_t** out) {
    ray_t* block = ray_alloc(sizeof(ray_term_t));
    if (!block) return NULL;
    ray_term_t* t = (ray_term_t*)ray_data(block);
    memset(t, 0, sizeof(*t));
    t->_block = block;
    t->term_width = 80;
    t->term_height = 24;
    t->last_total_rows = 1;
    ray_hist_create(&t->hist);
    *out = t;
    return block;
}

static void term_block_free(ray_t* block, ray_term_t* t) {
    ray_hist_destroy(&t->hist);
    ray_free(block);
}

/* Feed a single byte through ray_term_feed and return the produced ray_t
 * (or NULL).  Captures all stdout writes into a discard buffer. */
static ray_t* feed_byte(ray_term_t* t, int byte) {
    char path[256], cap[1024];
    int saved = capture_begin(path, sizeof path);
    t->input[0] = (char)(unsigned char)byte;
    ray_t* r = ray_term_feed(t);
    fflush(stdout);
    if (saved >= 0) (void)capture_end(saved, path, cap, sizeof cap);
    return r;
}

/* Feed a string of bytes (each fed as a separate keystroke).  Returns
 * the first non-NULL ray_t result, or NULL if none. */
static ray_t* feed_str(ray_term_t* t, const char* s) {
    ray_t* result = NULL;
    while (*s) {
        ray_t* r = feed_byte(t, (unsigned char)*s);
        if (r && !result) result = r;
        s++;
    }
    return result;
}

/* ─── ray_term_feed: printable keystrokes + Enter ─────────────────── */

static test_result_t test_term_feed_printable_then_enter(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* Type "hi", then Enter — should yield a ray_t string "hi". */
    feed_byte(t, 'h');
    TEST_ASSERT_EQ_I(t->buf_len, 1);
    TEST_ASSERT_EQ_I(t->buf[0], 'h');
    feed_byte(t, 'i');
    TEST_ASSERT_EQ_I(t->buf_len, 2);
    TEST_ASSERT_EQ_I(t->buf_pos, 2);

    ray_t* line = feed_byte(t, KEYCODE_RETURN);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_FMT(!RAY_IS_ERR(line), "got ERR object back");
    /* History must now hold "hi" */
    TEST_ASSERT_EQ_I(t->hist.count, 1);
    TEST_ASSERT_STR_EQ(t->hist.entries[0], "hi");

    ray_release(line);
    term_block_free(block, t);
    PASS();
}

/* Enter on empty buffer yields an empty-string ray_t (not NULL, not EOF). */
static test_result_t test_term_feed_enter_empty(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_t* line = feed_byte(t, KEYCODE_RETURN);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_FMT(!RAY_IS_ERR(line), "got ERR object back");
    TEST_ASSERT_EQ_I((int)ray_str_len(line), 0);
    ray_release(line);
    /* Empty submission must NOT add to history */
    TEST_ASSERT_EQ_I(t->hist.count, 0);

    term_block_free(block, t);
    PASS();
}

/* Enter inside an unmatched paren goes into multiline continuation mode:
 * feed returns NULL, the line is moved to multiline_buf with a trailing
 * '\n', and the next Enter (after closing) returns the full block. */
static test_result_t test_term_feed_multiline_continuation(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* "(a" then Enter — multiline_buf becomes "(a\n", buf cleared. */
    feed_byte(t, '(');
    feed_byte(t, 'a');
    ray_t* r = feed_byte(t, KEYCODE_RETURN);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_EQ_I(t->multiline_len, 3);
    TEST_ASSERT_FMT(t->multiline_buf[0] == '(' &&
                    t->multiline_buf[1] == 'a' &&
                    t->multiline_buf[2] == '\n',
                    "multiline_buf wrong: %.*s",
                    t->multiline_len, t->multiline_buf);
    TEST_ASSERT_EQ_I(t->buf_len, 0);

    /* Now type "b)" and press Enter — full block delivered. */
    feed_byte(t, 'b');
    feed_byte(t, ')');
    ray_t* line = feed_byte(t, KEYCODE_RETURN);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_FMT(!RAY_IS_ERR(line), "got ERR back");
    TEST_ASSERT_EQ_I((int)ray_str_len(line), 5);
    TEST_ASSERT_FMT(memcmp(ray_str_ptr(line), "(a\nb)", 5) == 0,
                    "multiline result wrong: %.*s",
                    (int)ray_str_len(line), ray_str_ptr(line));
    ray_release(line);

    /* multiline_len reset after delivery */
    TEST_ASSERT_EQ_I(t->multiline_len, 0);

    term_block_free(block, t);
    PASS();
}

/* Backspace removes the previous byte (pre-cursor); at empty buf is no-op. */
static test_result_t test_term_feed_backspace(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* No-op at start */
    feed_byte(t, KEYCODE_BACKSPACE);
    TEST_ASSERT_EQ_I(t->buf_len, 0);

    feed_str(t, "abc");
    TEST_ASSERT_EQ_I(t->buf_len, 3);
    feed_byte(t, KEYCODE_BACKSPACE);
    TEST_ASSERT_EQ_I(t->buf_len, 2);
    TEST_ASSERT_EQ_I(t->buf_pos, 2);
    TEST_ASSERT_FMT(memcmp(t->buf, "ab", 2) == 0, "buf=%c%c", t->buf[0], t->buf[1]);

    /* DEL (0x7f) is treated identically to backspace */
    feed_byte(t, KEYCODE_DELETE);
    TEST_ASSERT_EQ_I(t->buf_len, 1);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-A jumps to start, Ctrl-E jumps to end. */
static test_result_t test_term_feed_ctrl_a_e(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "hello");
    TEST_ASSERT_EQ_I(t->buf_pos, 5);
    feed_byte(t, KEYCODE_CTRL_A);
    TEST_ASSERT_EQ_I(t->buf_pos, 0);
    feed_byte(t, KEYCODE_CTRL_E);
    TEST_ASSERT_EQ_I(t->buf_pos, 5);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-K kills from cursor to end of line. */
static test_result_t test_term_feed_ctrl_k(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "abcdef");
    /* Position cursor at index 3 directly — Ctrl-K should truncate buf. */
    t->buf_pos = 3;
    feed_byte(t, KEYCODE_CTRL_K);
    TEST_ASSERT_EQ_I(t->buf_len, 3);
    TEST_ASSERT_FMT(memcmp(t->buf, "abc", 3) == 0, "buf wrong");

    term_block_free(block, t);
    PASS();
}

/* Ctrl-U clears the entire line. */
static test_result_t test_term_feed_ctrl_u(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "junk");
    feed_byte(t, KEYCODE_CTRL_U);
    TEST_ASSERT_EQ_I(t->buf_len, 0);
    TEST_ASSERT_EQ_I(t->buf_pos, 0);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-W deletes the previous word (non-alphanum + alphanum span). */
static test_result_t test_term_feed_ctrl_w(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "foo bar baz");
    /* Cursor at end (pos=11).  feed_normal's Ctrl-W skips trailing
     * non-alphanum, then back over alphanum — for "foo bar baz" with
     * cursor on the last 'z' (alphanum), the first while loop is a
     * no-op, then the second kills "baz", leaving "foo bar " (8 chars). */
    feed_byte(t, KEYCODE_CTRL_W);
    TEST_ASSERT_EQ_I(t->buf_len, 8);
    TEST_ASSERT_FMT(memcmp(t->buf, "foo bar ", 8) == 0,
                    "after first ^W buf='%.*s'", t->buf_len, t->buf);
    /* Again — first loop skips " " (non-alphanum), then kills "bar" -> "foo " */
    feed_byte(t, KEYCODE_CTRL_W);
    TEST_ASSERT_EQ_I(t->buf_len, 4);
    TEST_ASSERT_FMT(memcmp(t->buf, "foo ", 4) == 0,
                    "after 2nd ^W buf='%.*s'", t->buf_len, t->buf);
    /* Once more — kills "foo" with the leading nothing -> "" */
    feed_byte(t, KEYCODE_CTRL_W);
    TEST_ASSERT_EQ_I(t->buf_len, 0);
    /* No-op at empty buf */
    feed_byte(t, KEYCODE_CTRL_W);
    TEST_ASSERT_EQ_I(t->buf_len, 0);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-D on empty buf returns EOF; with content acts as forward-delete. */
static test_result_t test_term_feed_ctrl_d(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* Empty -> EOF */
    ray_t* r = feed_byte(t, KEYCODE_CTRL_D);
    TEST_ASSERT_FMT(r == RAY_TERM_EOF, "expected EOF marker, got %p", (void*)r);

    /* With content + cursor mid-buf: forward-delete */
    feed_str(t, "abcd");
    t->buf_pos = 1;
    feed_byte(t, KEYCODE_CTRL_D);
    TEST_ASSERT_EQ_I(t->buf_len, 3);
    TEST_ASSERT_FMT(memcmp(t->buf, "acd", 3) == 0, "buf=%.*s", t->buf_len, t->buf);
    /* Cursor at end — Ctrl-D becomes a no-op (only deletes when buf_pos<len) */
    t->buf_pos = t->buf_len;
    feed_byte(t, KEYCODE_CTRL_D);
    TEST_ASSERT_EQ_I(t->buf_len, 3);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-C clears the line, prints "^C\n" + a fresh prompt. */
static test_result_t test_term_feed_ctrl_c(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "junk");
    char path[256], cap[2048];
    int saved = capture_begin(path, sizeof path);
    t->input[0] = KEYCODE_CTRL_C;
    ray_t* r = ray_term_feed(t);
    fflush(stdout);
    int32_t n = capture_end(saved, path, cap, sizeof cap);
    TEST_ASSERT_NULL(r);
    TEST_ASSERT_EQ_I(t->buf_len, 0);
    TEST_ASSERT_EQ_I(t->buf_pos, 0);
    TEST_ASSERT_EQ_I(t->multiline_len, 0);
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_FMT(n > 0, "no output");
    TEST_ASSERT_FMT(strstr(cap, "^C") != NULL, "missing ^C marker");

    term_block_free(block, t);
    PASS();
}

/* ─── ray_term_feed: escape sequences ─────────────────────────────── */

/* ESC [ A = Up arrow.  Pre-populates history; expects buf to load with the
 * previous entry. */
static test_result_t test_term_feed_arrow_up(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "older",  5);
    ray_hist_add(&t->hist, "newer",  5);

    feed_byte(t, 0x1b);  /* ESC */
    TEST_ASSERT_EQ_I(t->esc_state, 1);
    feed_byte(t, '[');
    TEST_ASSERT_EQ_I(t->esc_state, 2);
    feed_byte(t, 'A');   /* up — loads "newer" */
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 5);
    TEST_ASSERT_FMT(memcmp(t->buf, "newer", 5) == 0,
                    "after up: %.*s", t->buf_len, t->buf);

    /* Another up loads "older" */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'A');
    TEST_ASSERT_FMT(memcmp(t->buf, "older", 5) == 0,
                    "after 2nd up: %.*s", t->buf_len, t->buf);

    /* Down arrow goes back forward */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'B');
    TEST_ASSERT_FMT(memcmp(t->buf, "newer", 5) == 0,
                    "after down: %.*s", t->buf_len, t->buf);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-P / Ctrl-N have the same semantics as Up / Down. */
static test_result_t test_term_feed_ctrl_pn(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "histent", 7);

    feed_byte(t, KEYCODE_CTRL_P);
    TEST_ASSERT_FMT(memcmp(t->buf, "histent", 7) == 0,
                    "Ctrl-P should load 'histent', got '%.*s'",
                    t->buf_len, t->buf);
    feed_byte(t, KEYCODE_CTRL_N);
    TEST_ASSERT_EQ_I(t->buf_len, 0);

    term_block_free(block, t);
    PASS();
}

/* ESC [ D = Left, ESC [ C = Right.  Right-at-end with no ghost is a no-op. */
static test_result_t test_term_feed_arrow_left_right(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* Use chars unlikely to trigger ghost completion — "qzqzj" doesn't
     * match any env builtin or history-word prefix in a fresh runtime. */
    feed_str(t, "qzqzj");
    TEST_ASSERT_EQ_I(t->buf_pos, 5);

    /* Left */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'D');
    TEST_ASSERT_EQ_I(t->buf_pos, 4);

    /* Right */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'C');
    TEST_ASSERT_EQ_I(t->buf_pos, 5);

    /* Right past end with no ghost: no-op */
    int32_t len_before = t->buf_len;
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'C');
    TEST_ASSERT_EQ_I(t->buf_pos, 5);
    TEST_ASSERT_EQ_I(t->buf_len, len_before);

    /* Left at start — no-op */
    t->buf_pos = 0;
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'D');
    TEST_ASSERT_EQ_I(t->buf_pos, 0);

    term_block_free(block, t);
    PASS();
}

/* ESC [ H = Home, ESC [ F = End. */
static test_result_t test_term_feed_home_end(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "xyz");
    /* Home (CSI H) */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'H');
    TEST_ASSERT_EQ_I(t->buf_pos, 0);
    /* End (CSI F) */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'F');
    TEST_ASSERT_EQ_I(t->buf_pos, 3);
    /* SS3 variants: ESC O H / ESC O F */
    feed_byte(t, 0x1b); feed_byte(t, 'O'); feed_byte(t, 'H');
    TEST_ASSERT_EQ_I(t->buf_pos, 0);
    feed_byte(t, 0x1b); feed_byte(t, 'O'); feed_byte(t, 'F');
    TEST_ASSERT_EQ_I(t->buf_pos, 3);
    /* SS3 with unknown final byte — exits state, no-op */
    feed_byte(t, 0x1b); feed_byte(t, 'O'); feed_byte(t, 'X');
    TEST_ASSERT_EQ_I(t->esc_state, 0);

    term_block_free(block, t);
    PASS();
}

/* ESC [ 3 ~ = Delete (forward delete). */
static test_result_t test_term_feed_csi_delete(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "abcd");
    t->buf_pos = 1;
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, '3');
    TEST_ASSERT_EQ_I(t->esc_state, 4);
    feed_byte(t, '~');
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 3);
    TEST_ASSERT_FMT(memcmp(t->buf, "acd", 3) == 0, "buf=%.*s", t->buf_len, t->buf);

    /* "ESC [ 3 X" (X != ~) — exits cleanly without deletion */
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, '3'); feed_byte(t, 'X');
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 3);

    /* CSI Delete at end — no-op */
    t->buf_pos = t->buf_len;
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, '3'); feed_byte(t, '~');
    TEST_ASSERT_EQ_I(t->buf_len, 3);

    term_block_free(block, t);
    PASS();
}

/* Bare ESC followed by an unrecognised byte returns to normal state.  No
 * crash, no change to buf. */
static test_result_t test_term_feed_bare_escape(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "hi");
    feed_byte(t, 0x1b);
    TEST_ASSERT_EQ_I(t->esc_state, 1);
    /* Unrecognised continuation: 'X' — must reset state cleanly. */
    feed_byte(t, 'X');
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 2);

    term_block_free(block, t);
    PASS();
}

/* Unknown CSI is consumed until the final byte (anything in [0x40, 0x7E]). */
static test_result_t test_term_feed_unknown_csi(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "x");
    /* "ESC [ 1 ; 2 R" — unhandled: should be consumed without modifying buf. */
    feed_byte(t, 0x1b); feed_byte(t, '[');
    feed_byte(t, '1'); /* not a recognised final or special, becomes state 5 */
    TEST_ASSERT_EQ_I(t->esc_state, 5);
    feed_byte(t, ';');
    feed_byte(t, '2');
    feed_byte(t, 'R'); /* final byte (0x52) terminates */
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 1);  /* original 'x' untouched */

    /* Long unknown CSI — overflow guard kicks in after >8 bytes */
    feed_byte(t, 0x1b); feed_byte(t, '[');
    feed_byte(t, '?'); /* enters state 5 */
    TEST_ASSERT_EQ_I(t->esc_state, 5);
    for (int i = 0; i < 12; i++) feed_byte(t, '0');
    TEST_ASSERT_EQ_I(t->esc_state, 0);

    term_block_free(block, t);
    PASS();
}

/* When a printable char arrives mid-buffer, it should be inserted at
 * cursor_pos (not appended). */
static test_result_t test_term_feed_insert_mid_buffer(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    feed_str(t, "ab");
    t->buf_pos = 1;       /* between 'a' and 'b' */
    feed_byte(t, 'X');
    TEST_ASSERT_EQ_I(t->buf_len, 3);
    TEST_ASSERT_FMT(memcmp(t->buf, "aXb", 3) == 0,
                    "expected aXb got %.*s", t->buf_len, t->buf);
    TEST_ASSERT_EQ_I(t->buf_pos, 2);  /* advanced past inserted char */

    term_block_free(block, t);
    PASS();
}

/* ─── ray_term_feed: Tab completion / ghost ───────────────────────── */

/* Tab with multiple candidates enters cycling mode; subsequent Tabs
 * advance the cycle. */
static test_result_t test_term_feed_tab_cycling(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* History gives at least 2 distinct words starting with "uniq" */
    ray_hist_add(&t->hist, "uniqaa one", 10);
    ray_hist_add(&t->hist, "uniqbb two", 10);

    feed_str(t, "uniq");
    /* Sanity: at this point the redraw should have populated comp_count
     * with the two history matches. */
    TEST_ASSERT_FMT(t->comp_count >= 2,
                    "expected >=2 completions before Tab, got %d", t->comp_count);

    feed_byte(t, KEYCODE_TAB);
    /* After first Tab the cycling flag is set and a candidate has been
     * inserted (replacing the typed prefix).  Note: update_ghost is then
     * called again on the now-complete word, which often clears comp_count
     * to 0 — that's expected.  We assert on comp_cycling instead. */
    TEST_ASSERT_FMT(t->comp_cycling == 1,
                    "expected cycling=1 after Tab");
    /* Buffer should now hold one of the two matches in full */
    int matched = (t->buf_len == 6 && memcmp(t->buf, "uniqaa", 6) == 0)
               || (t->buf_len == 6 && memcmp(t->buf, "uniqbb", 6) == 0);
    TEST_ASSERT_FMT(matched, "expected uniqaa/uniqbb after Tab; got '%.*s'",
                    t->buf_len, t->buf);
    int after_first_idx = t->comp_cycle_idx;

    /* Second Tab cycles to next candidate.  When cycling is active the
     * stored comp_items aren't recomputed — the cycle just advances. */
    feed_byte(t, KEYCODE_TAB);
    TEST_ASSERT_FMT(t->comp_cycle_idx != after_first_idx,
                    "cycle_idx didn't advance: %d", t->comp_cycle_idx);

    /* Any non-Tab key resets cycling */
    feed_byte(t, 'x');
    TEST_ASSERT_EQ_I(t->comp_cycling, 0);

    term_block_free(block, t);
    PASS();
}

/* Tab with a single ghost candidate accepts the ghost. */
static test_result_t test_term_feed_tab_single_ghost(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* Just one word starting with "qzqz" in history. */
    ray_hist_add(&t->hist, "qzqzunique end", 14);
    feed_str(t, "qzqz");
    /* feed_str redraw populates ghost+comp_count; if comp_count==1 ghost
     * accepted by Tab.  This is the single-completion path. */
    TEST_ASSERT_FMT(t->comp_count == 1,
                    "expected 1 completion, got %d", t->comp_count);
    int prev_len = t->buf_len;
    feed_byte(t, KEYCODE_TAB);
    TEST_ASSERT_FMT(t->buf_len > prev_len,
                    "Tab should have extended buf from %d to %d",
                    prev_len, t->buf_len);
    TEST_ASSERT_FMT(memcmp(t->buf, "qzqzunique", 10) == 0,
                    "buf=%.*s", t->buf_len, t->buf);

    term_block_free(block, t);
    PASS();
}

/* Right arrow at end-of-buffer with a ghost text accepts the ghost. */
static test_result_t test_term_feed_right_accepts_ghost(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "spectreword done", 16);
    feed_str(t, "spect");
    /* Cursor at end; ghost should be set. */
    TEST_ASSERT_FMT(t->ghost_len > 0,
                    "expected ghost to be set for 'spect'");
    int before = t->buf_len;
    feed_byte(t, 0x1b); feed_byte(t, '['); feed_byte(t, 'C'); /* Right */
    TEST_ASSERT_FMT(t->buf_len > before,
                    "Right at end should accept ghost; before=%d after=%d",
                    before, t->buf_len);

    term_block_free(block, t);
    PASS();
}

/* ESC alone after Tab cycling cancels the cycle (and redraws). */
static test_result_t test_term_feed_esc_cancels_tab(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "abca one", 8);
    ray_hist_add(&t->hist, "abcb two", 8);
    feed_str(t, "abc");
    feed_byte(t, KEYCODE_TAB);
    TEST_ASSERT_EQ_I(t->comp_cycling, 1);

    /* Bare ESC: state-1 enters, then any non-CSI byte exits.  Easiest
     * way: send ESC followed by a non-recognised byte. */
    feed_byte(t, 0x1b);
    feed_byte(t, 'q');  /* unrecognised continuation */
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    TEST_ASSERT_EQ_I(t->comp_cycling, 0);

    term_block_free(block, t);
    PASS();
}

/* ─── ray_term_feed: search mode ──────────────────────────────────── */

/* Ctrl-R enters search mode and prints the search prompt. */
static test_result_t test_term_feed_ctrl_r_enters_search(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    char path[256], cap[2048];
    int saved = capture_begin(path, sizeof path);
    t->input[0] = KEYCODE_CTRL_R;
    ray_term_feed(t);
    fflush(stdout);
    int32_t n = capture_end(saved, path, cap, sizeof cap);
    TEST_ASSERT_EQ_I(t->search_mode, 1);
    TEST_ASSERT_EQ_I(t->search_len, 0);
    TEST_ASSERT_EQ_I(t->search_match_idx, -1);
    TEST_ASSERT_FMT(n > 0, "no search prompt output");
    TEST_ASSERT_FMT(strstr(cap, "(search)") != NULL,
                    "missing '(search)' marker in output");

    term_block_free(block, t);
    PASS();
}

/* Type characters in search mode: builds up search_buf and finds matches. */
static test_result_t test_term_feed_search_typing(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "hello world", 11);
    ray_hist_add(&t->hist, "goodbye now", 11);
    feed_byte(t, KEYCODE_CTRL_R);
    TEST_ASSERT_EQ_I(t->search_mode, 1);

    feed_byte(t, 'h');
    TEST_ASSERT_EQ_I(t->search_len, 1);
    TEST_ASSERT_FMT(t->search_match_idx == 0,
                    "expected match at idx 0, got %d", t->search_match_idx);
    feed_byte(t, 'e');
    TEST_ASSERT_EQ_I(t->search_len, 2);
    TEST_ASSERT_FMT(t->search_match_idx == 0,
                    "expected match at idx 0, got %d", t->search_match_idx);

    /* Backspace shrinks query */
    feed_byte(t, KEYCODE_BACKSPACE);
    TEST_ASSERT_EQ_I(t->search_len, 1);
    /* Backspace down to zero clears match_idx */
    feed_byte(t, KEYCODE_BACKSPACE);
    TEST_ASSERT_EQ_I(t->search_len, 0);
    TEST_ASSERT_EQ_I(t->search_match_idx, -1);
    /* Backspace at empty is a no-op */
    feed_byte(t, KEYCODE_BACKSPACE);
    TEST_ASSERT_EQ_I(t->search_len, 0);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-R while in search mode searches further back. */
static test_result_t test_term_feed_search_repeat(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "select foo",  10);
    ray_hist_add(&t->hist, "select bar",  10);
    feed_byte(t, KEYCODE_CTRL_R);
    feed_str(t, "sel");
    TEST_ASSERT_EQ_I(t->search_match_idx, 1);  /* finds "select bar" */

    /* Second Ctrl-R goes further back */
    feed_byte(t, KEYCODE_CTRL_R);
    TEST_ASSERT_EQ_I(t->search_match_idx, 0);

    /* No older match -> stays at 0 */
    feed_byte(t, KEYCODE_CTRL_R);
    TEST_ASSERT_EQ_I(t->search_match_idx, 0);

    term_block_free(block, t);
    PASS();
}

/* Enter inside search mode accepts the current match into the buffer
 * and submits it as a line. */
static test_result_t test_term_feed_search_accept(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "match_target_str", 16);
    feed_byte(t, KEYCODE_CTRL_R);
    feed_str(t, "target");
    TEST_ASSERT_EQ_I(t->search_match_idx, 0);

    ray_t* line = feed_byte(t, KEYCODE_RETURN);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQ_I(t->search_mode, 0);
    TEST_ASSERT_EQ_I((int)ray_str_len(line), 16);
    TEST_ASSERT_FMT(memcmp(ray_str_ptr(line), "match_target_str", 16) == 0,
                    "line bytes wrong: %.*s",
                    (int)ray_str_len(line), ray_str_ptr(line));
    ray_release(line);

    term_block_free(block, t);
    PASS();
}

/* Ctrl-C inside search mode exits search and clears buf. */
static test_result_t test_term_feed_search_ctrl_c(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "abc", 3);
    feed_byte(t, KEYCODE_CTRL_R);
    feed_str(t, "ab");
    feed_byte(t, KEYCODE_CTRL_C);
    TEST_ASSERT_EQ_I(t->search_mode, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 0);

    term_block_free(block, t);
    PASS();
}

/* ESC inside search mode: exits search but enters the escape-state
 * machine so the following bytes (e.g. arrow continuation) are consumed
 * cleanly without being inserted into the buffer. */
static test_result_t test_term_feed_search_escape_then_arrow(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "older_one", 9);
    ray_hist_add(&t->hist, "newer_two", 9);
    feed_byte(t, KEYCODE_CTRL_R);
    feed_str(t, "older");
    TEST_ASSERT_EQ_I(t->search_match_idx, 0);

    /* ESC bytes — search exits, esc_state=1.  Then "[A" — but the way
     * the code sets esc_state=1 before consuming the actual ESC byte
     * means the caller is expected to send ESC again to drive the
     * state machine?  Re-read: feed_search on ESC sets esc_state=1
     * directly so that the subsequent bytes go through feed_escape.
     * So we send the introducer + final byte next. */
    feed_byte(t, KEYCODE_ESCAPE);
    TEST_ASSERT_EQ_I(t->search_mode, 0);
    TEST_ASSERT_EQ_I(t->esc_state, 1);
    /* Send the introducer + final byte: '[' 'A' = up arrow */
    feed_byte(t, '[');
    TEST_ASSERT_EQ_I(t->esc_state, 2);
    feed_byte(t, 'A');
    TEST_ASSERT_EQ_I(t->esc_state, 0);
    /* Up arrow nav loaded most-recent history entry */
    TEST_ASSERT_EQ_I(t->buf_len, 9);
    TEST_ASSERT_FMT(memcmp(t->buf, "newer_two", 9) == 0,
                    "expected newer_two, got %.*s", t->buf_len, t->buf);

    term_block_free(block, t);
    PASS();
}

/* Search with no match: search_match_idx stays at -1, Enter submits an
 * empty line (since buf was never populated). */
static test_result_t test_term_feed_search_no_match(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "nothing relevant", 16);
    feed_byte(t, KEYCODE_CTRL_R);
    feed_str(t, "qqqq");
    TEST_ASSERT_EQ_I(t->search_match_idx, -1);
    /* Enter still submits an empty line */
    ray_t* line = feed_byte(t, KEYCODE_RETURN);
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_EQ_I((int)ray_str_len(line), 0);
    ray_release(line);

    term_block_free(block, t);
    PASS();
}

/* ─── ray_term_search_redraw exercise ─────────────────────────────── */

/* Drives ray_term_search_redraw via a Ctrl-R + typed query and inspects
 * the captured stdout for the expected escape-coded match highlight. */
static test_result_t test_term_search_redraw_emits_highlight(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    ray_hist_add(&t->hist, "bingo result", 12);
    feed_byte(t, KEYCODE_CTRL_R);

    char path[256], cap[2048];
    int saved = capture_begin(path, sizeof path);
    /* Type the query — feed_search will call ray_term_search_redraw */
    t->input[0] = 'b';
    ray_term_feed(t);
    t->input[0] = 'i';
    ray_term_feed(t);
    fflush(stdout);
    int32_t n = capture_end(saved, path, cap, sizeof cap);
    TEST_ASSERT_FMT(n > 0, "no search redraw output");
    TEST_ASSERT_FMT(strstr(cap, "(search)") != NULL, "missing search prompt");
    TEST_ASSERT_FMT(strstr(cap, "\033[7m") != NULL,
                    "missing reverse-video match highlight (CSI 7m)");
    TEST_ASSERT_FMT(strstr(cap, "bingo result") != NULL ||
                    strstr(cap, "bi") != NULL,
                    "missing matched entry text");

    term_block_free(block, t);
    PASS();
}

/* ─── Multiline overflow ──────────────────────────────────────────── */

/* When the multiline buffer would overflow, the code writes a diagnostic
 * to stderr and resets state without pushing more.  Redirect both
 * stderr (for the diagnostic) and stdout (for the prompt redraw) so the
 * runner's output isn't polluted. */
static test_result_t test_term_feed_multiline_overflow(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* Arrange: pre-fill multiline_buf to nearly full, set buf to a string
     * that, when added, would exceed TERM_BUF_SIZE.  Buf has unmatched
     * paren so Enter chooses the continuation path. */
    int32_t fill = TERM_BUF_SIZE - 4;
    memset(t->multiline_buf, 'x', (size_t)fill);
    t->multiline_len = fill;
    t->buf[0] = '(';
    t->buf[1] = 'a';
    t->buf[2] = 'b';
    t->buf[3] = 'c';
    t->buf_len = 4;
    t->buf_pos = 4;

    /* Redirect stderr to /dev/null to suppress the "input too long" diag. */
    fflush(stderr);
    int saved_err = dup(fileno(stderr));
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, fileno(stderr));
        close(devnull);
    }

    ray_t* r = feed_byte(t, KEYCODE_RETURN);

    fflush(stderr);
    if (saved_err >= 0) {
        dup2(saved_err, fileno(stderr));
        close(saved_err);
    }

    TEST_ASSERT_NULL(r);
    /* On overflow path: multiline_len reset to 0, buf cleared. */
    TEST_ASSERT_EQ_I(t->multiline_len, 0);
    TEST_ASSERT_EQ_I(t->buf_len, 0);
    TEST_ASSERT_EQ_I(t->buf_pos, 0);

    term_block_free(block, t);
    PASS();
}

/* ─── End-of-buffer overflow on insert ────────────────────────────── */

/* feed_normal silently drops printable chars when buf is at TERM_BUF_SIZE-1.
 * Drive that path so the bounds check is exercised. */
static test_result_t test_term_feed_insert_overflow(void) {
    ray_term_t* t;
    ray_t* block = term_block_new(&t);
    if (!block) FAIL("term_block_new failed");

    /* Pre-fill buf with TERM_BUF_SIZE-1 bytes of 'a' so the next insert
     * is rejected. */
    memset(t->buf, 'a', TERM_BUF_SIZE - 1);
    t->buf_len = TERM_BUF_SIZE - 1;
    t->buf_pos = TERM_BUF_SIZE - 1;

    feed_byte(t, 'b');  /* should be ignored */
    TEST_ASSERT_EQ_I(t->buf_len, TERM_BUF_SIZE - 1);

    term_block_free(block, t);
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

    /* Prompt prefix + prompts */
    { "term/set_prompt_prefix",         test_term_set_prompt_prefix_basic,
      heap_setup, heap_teardown },
    { "term/prompt_emits_bytes",        test_term_prompt_emits_bytes,
      heap_setup, heap_teardown },
    { "term/prompt_with_prefix",        test_term_prompt_with_prefix,
      heap_setup, heap_teardown },
    { "term/continuation_prompt",       test_term_continuation_prompt,
      heap_setup, heap_teardown },

    /* Interrupt flag */
    { "term/interrupt_flag",            test_term_interrupt_flag,
      heap_setup, heap_teardown },

    /* Event-driven line editor — keystroke state machine.  Many of these
     * trigger ray_term_redraw which highlights words via ray_env_has_name,
     * so they need the full runtime. */
    { "term/begin_resets_state",        test_term_begin_resets_state,
      runtime_setup, runtime_teardown },

    { "term/feed_printable",            test_term_feed_printable_then_enter,
      runtime_setup, runtime_teardown },
    { "term/feed_enter_empty",          test_term_feed_enter_empty,
      runtime_setup, runtime_teardown },
    { "term/feed_multiline",            test_term_feed_multiline_continuation,
      runtime_setup, runtime_teardown },
    { "term/feed_backspace",            test_term_feed_backspace,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_a_e",             test_term_feed_ctrl_a_e,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_k",               test_term_feed_ctrl_k,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_u",               test_term_feed_ctrl_u,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_w",               test_term_feed_ctrl_w,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_d",               test_term_feed_ctrl_d,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_c",               test_term_feed_ctrl_c,
      runtime_setup, runtime_teardown },

    { "term/feed_arrow_up",             test_term_feed_arrow_up,
      runtime_setup, runtime_teardown },
    { "term/feed_ctrl_pn",              test_term_feed_ctrl_pn,
      runtime_setup, runtime_teardown },
    { "term/feed_arrow_left_right",     test_term_feed_arrow_left_right,
      runtime_setup, runtime_teardown },
    { "term/feed_home_end",             test_term_feed_home_end,
      runtime_setup, runtime_teardown },
    { "term/feed_csi_delete",           test_term_feed_csi_delete,
      runtime_setup, runtime_teardown },
    { "term/feed_bare_escape",          test_term_feed_bare_escape,
      runtime_setup, runtime_teardown },
    { "term/feed_unknown_csi",          test_term_feed_unknown_csi,
      runtime_setup, runtime_teardown },
    { "term/feed_insert_mid",           test_term_feed_insert_mid_buffer,
      runtime_setup, runtime_teardown },

    { "term/feed_tab_cycle",            test_term_feed_tab_cycling,
      runtime_setup, runtime_teardown },
    { "term/feed_tab_single",           test_term_feed_tab_single_ghost,
      runtime_setup, runtime_teardown },
    { "term/feed_right_ghost",          test_term_feed_right_accepts_ghost,
      runtime_setup, runtime_teardown },
    { "term/feed_esc_cancels_tab",      test_term_feed_esc_cancels_tab,
      runtime_setup, runtime_teardown },

    /* Search mode */
    { "term/feed_ctrl_r_enters",        test_term_feed_ctrl_r_enters_search,
      runtime_setup, runtime_teardown },
    { "term/feed_search_typing",        test_term_feed_search_typing,
      runtime_setup, runtime_teardown },
    { "term/feed_search_repeat",        test_term_feed_search_repeat,
      runtime_setup, runtime_teardown },
    { "term/feed_search_accept",        test_term_feed_search_accept,
      runtime_setup, runtime_teardown },
    { "term/feed_search_ctrl_c",        test_term_feed_search_ctrl_c,
      runtime_setup, runtime_teardown },
    { "term/feed_search_esc_arrow",     test_term_feed_search_escape_then_arrow,
      runtime_setup, runtime_teardown },
    { "term/feed_search_no_match",      test_term_feed_search_no_match,
      runtime_setup, runtime_teardown },
    { "term/search_redraw_highlight",   test_term_search_redraw_emits_highlight,
      runtime_setup, runtime_teardown },

    /* Overflow paths */
    { "term/feed_multiline_overflow",   test_term_feed_multiline_overflow,
      runtime_setup, runtime_teardown },
    { "term/feed_insert_overflow",      test_term_feed_insert_overflow,
      runtime_setup, runtime_teardown },

    { NULL, NULL, NULL, NULL },
};
