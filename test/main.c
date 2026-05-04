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
 * Rayforce test driver — v1-style, zero third-party.
 *
 * Each test file exports a `const test_entry_t FOO_entries[]` terminated
 * by a { NULL, ... } sentinel.  main.c aggregates those arrays plus a
 * dynamic set of entries discovered by walking test/rfl recursively at
 * startup.  Tests are run sequentially with per-entry setup/teardown
 * and per-test timing.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include "test_rfl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rayforce.h>
#include "lang/eval.h"
#include "lang/format.h"

/* Forward-declare runtime API — mirrors existing test_lang.c pattern. */
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
extern ray_runtime_t* __RUNTIME;

/* ─── Shared state ────────────────────────────────────────────────── */

char    ray_test_fail_buf[2048];
jmp_buf ray_test_jmp;
int     ray_test_jmp_active = 0;

#include <stdarg.h>

void ray_test_fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ray_test_fail_buf, sizeof ray_test_fail_buf, fmt, ap);
    va_end(ap);
    if (ray_test_jmp_active) {
        longjmp(ray_test_jmp, 1);
    }
    fprintf(stderr, "test driver: ray_test_fatal called outside a test: %s\n",
            ray_test_fail_buf);
    abort();
}

/* ─── ANSI colors (disabled when not a TTY) ──────────────────────── */

static int g_color = 0;
#define C_RESET   (g_color ? "\x1b[0m"  : "")
#define C_GREEN   (g_color ? "\x1b[32m" : "")
#define C_RED     (g_color ? "\x1b[31m" : "")
#define C_YELLOW  (g_color ? "\x1b[33m" : "")
#define C_CYAN    (g_color ? "\x1b[36m" : "")
#define C_GRAY    (g_color ? "\x1b[90m" : "")

/* ─── Compiled-in test groups ─────────────────────────────────────── */
/* Each test_*.c file exposes one of these; add a new line here when a
 * new file is added.  Sentinel-terminated (final entry has NULL name). */

extern const test_entry_t err_entries[];
extern const test_entry_t arena_entries[];
extern const test_entry_t atom_entries[];
extern const test_entry_t audit_entries[];
extern const test_entry_t block_entries[];
extern const test_entry_t buddy_entries[];
extern const test_entry_t cow_entries[];
extern const test_entry_t csr_entries[];
extern const test_entry_t csv_entries[];
extern const test_entry_t datalog_entries[];
extern const test_entry_t dict_entries[];
extern const test_entry_t dump_entries[];
extern const test_entry_t embedding_entries[];
extern const test_entry_t exec_entries[];
extern const test_entry_t format_entries[];
extern const test_entry_t fvec_entries[];
extern const test_entry_t graph_entries[];
extern const test_entry_t graph_builtin_entries[];
extern const test_entry_t group_extra_entries[];
extern const test_entry_t heap_entries[];
extern const test_entry_t index_entries[];
extern const test_entry_t ipc_entries[];
extern const test_entry_t journal_entries[];
extern const test_entry_t lang_entries[];
extern const test_entry_t link_entries[];
extern const test_entry_t lftj_entries[];
extern const test_entry_t list_entries[];
extern const test_entry_t meta_entries[];
extern const test_entry_t morsel_entries[];
extern const test_entry_t numparse_entries[];
extern const test_entry_t opt_entries[];
extern const test_entry_t partition_exec_entries[];
extern const test_entry_t pipe_entries[];
extern const test_entry_t platform_entries[];
extern const test_entry_t pool_entries[];
extern const test_entry_t progress_entries[];
extern const test_entry_t repl_entries[];
extern const test_entry_t rowsel_entries[];
extern const test_entry_t runtime_entries[];
extern const test_entry_t sel_entries[];
extern const test_entry_t splay_entries[];
extern const test_entry_t store_entries[];
extern const test_entry_t str_entries[];
extern const test_entry_t sym_entries[];
extern const test_entry_t sys_entries[];
extern const test_entry_t table_entries[];
extern const test_entry_t term_entries[];
extern const test_entry_t types_entries[];
extern const test_entry_t vec_entries[];
extern const test_entry_t window_entries[];

static const test_entry_t* const compiled_groups[] = {
    err_entries,      arena_entries,    atom_entries,     audit_entries,
    block_entries,    buddy_entries,    cow_entries,      csr_entries,
    csv_entries,      datalog_entries,  dict_entries,     dump_entries,
    embedding_entries, exec_entries,
    format_entries,   fvec_entries,     graph_entries,    graph_builtin_entries,
    group_extra_entries,
    heap_entries,
    index_entries,    ipc_entries,
    journal_entries,
    lang_entries,     link_entries,
    lftj_entries,     list_entries,     meta_entries,     morsel_entries,
    numparse_entries, opt_entries,      partition_exec_entries,
    pipe_entries,     platform_entries,
    pool_entries,     progress_entries,
    repl_entries,     rowsel_entries,   runtime_entries,  sel_entries,
    splay_entries,    store_entries,
    str_entries,      sym_entries,      sys_entries,      table_entries,
    term_entries,
    types_entries,    vec_entries,      window_entries,
    NULL,
};

/* ─── .rfl auto-discovery ─────────────────────────────────────────── */
/*
 * A pool of pre-declared thunks dispatches loaded .rfl files by index.
 * Each thunk calls run_rfl_at(N) which loads the file at that slot and
 * evaluates it under a fresh runtime (via rfl_setup/rfl_teardown).
 */

#define RFL_THUNK_CAPACITY 256

static char  g_rfl_paths[RFL_THUNK_CAPACITY][512];
static char  g_rfl_names[RFL_THUNK_CAPACITY][256];
static int   g_rfl_count = 0;

/*
 * .rfl file semantics (line-based, v1-style TEST_ASSERT_EQ/TEST_ASSERT_ER):
 *
 *   LHS -- RHS     evaluate both as Rayfall, format both, string-compare.
 *                  Failure reports file:line with both formatted values.
 *   EXPR !- SUBSTR evaluate EXPR; expect a RAY_ERROR whose formatted text
 *                  contains SUBSTR.  Failure if no error or wrong error.
 *   ;; comment     ignored.
 *   blank          ignored.
 *   EXPR           raw Rayfall — evaluate; error = test failure.
 *                  Typical use is `(set x ...)` setup between assertions.
 *
 * Each line must be self-contained (no multi-line expressions).  Global
 * state set via `(set x ...)` persists across lines — the runtime is live
 * for the whole file.
 *
 * Limitation: the literal " -- " / " !- " sequences mustn't appear inside
 * Rayfall string literals on the same line.  Rewrite as separate lines or
 * use a setup variable if you need them.
 */

static int fmt_eq(ray_t* a, ray_t* b) {
    /* ray_eval_str returns NULL for void results (like evaluating `null`).
     * Two NULLs format-equal; one NULL differs from anything else. */
    if (a == NULL && b == NULL) return 1;
    if (a == NULL || b == NULL) return 0;
    ray_t* sa = ray_fmt(a, 0);
    ray_t* sb = ray_fmt(b, 0);
    int eq = sa && sb
          && ray_str_len(sa) == ray_str_len(sb)
          && memcmp(ray_str_ptr(sa), ray_str_ptr(sb), ray_str_len(sa)) == 0;
    if (sa) ray_release(sa);
    if (sb) ray_release(sb);
    return eq;
}

static void fmt_into(ray_t* v, char* out, size_t cap) {
    ray_t* s = v ? ray_fmt(v, 0) : NULL;
    size_t n = s ? ray_str_len(s) : 0;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(out, ray_str_ptr(s), n);
    out[n] = '\0';
    if (s) ray_release(s);
}

/* Trim trailing whitespace in-place.  Returns new length. */
static size_t rstrip(char* s, size_t len) {
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t'
                    || s[len-1] == '\r' || s[len-1] == '\n'))
        len--;
    s[len] = '\0';
    return len;
}

/* Return pointer to first non-whitespace in [p, p+len) (or NULL). */
static char* lstrip(char* p, size_t len) {
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\t')) i++;
    return (i < len) ? (p + i) : NULL;
}

/* Find `marker` at the top level of `s`, honoring string literals (so a
 * separator inside `"a -- b"` is not matched).  Returns pointer to the
 * match in `s`, or NULL.  Uses strncmp so reads past the nul-terminator
 * cannot occur — a tail byte too short to hold the full marker simply
 * mismatches and loop falls through to the `*p` guard. */
static char* find_top_sep(char* s, const char* marker) {
    size_t mlen   = strlen(marker);
    int    in_str = 0;
    int    esc    = 0;
    for (char* p = s; *p; p++) {
        char c = *p;
        if (esc) { esc = 0; continue; }
        if (c == '\\') { esc = 1; continue; }
        if (c == '"') { in_str = !in_str; continue; }
        if (in_str) continue;
        if (strncmp(p, marker, mlen) == 0) return p;
    }
    return NULL;
}

static test_result_t run_rfl_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) FAILF("cannot open %s", path);
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); FAILF("fseek failed on %s", path); }
    long n = ftell(f);
    if (n < 0) { fclose(f); FAILF("ftell failed on %s", path); }
    rewind(f);
    char* src = (char*)malloc((size_t)n + 1);
    if (!src) { fclose(f); FAIL("oom"); }
    size_t r = fread(src, 1, (size_t)n, f);
    src[r] = '\0';
    fclose(f);

    int   line_no       = 0;
    int   assert_count  = 0;  /* tallies LHS -- RHS and EXPR !- SUBSTR lines */
    char* p             = src;
    test_result_t res   = { TEST_PASS, NULL };

    while (*p) {
        char* nl_ptr = strchr(p, '\n');
        size_t line_len = nl_ptr ? (size_t)(nl_ptr - p) : strlen(p);
        line_no++;

        /* Nul-terminate the line for strstr/eval convenience. */
        char saved_nl = nl_ptr ? *nl_ptr : '\0';
        if (nl_ptr) *nl_ptr = '\0';

        /* Rstrip trailing whitespace. */
        line_len = rstrip(p, line_len);

        /* Find start of non-whitespace; skip blank and ;; comment lines. */
        char* start = lstrip(p, line_len);
        if (!start || (start[0] == ';' && start[1] == ';')) {
            goto next;
        }

        /* Look for " -- " / " !- " (assertion markers).  String-literal
         * aware so the separator inside a Rayfall string isn't matched. */
        char* eq = find_top_sep(start, " -- ");
        char* er = find_top_sep(start, " !- ");

        if (eq) {
            assert_count++;
            *eq = '\0';
            char* lhs = start;
            char* rhs = eq + 4;
            ray_t* le = ray_eval_str(lhs);
            if (RAY_IS_ERR(le)) {
                char buf[512]; fmt_into(le, buf, sizeof buf);
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "%s:%d: LHS eval error: %s  -- src: %s",
                         path, line_no, buf, lhs);
                ray_error_free(le);  /* ray_release is a no-op on errors */
                res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                goto done;
            }
            ray_t* re = ray_eval_str(rhs);
            if (RAY_IS_ERR(re)) {
                char buf[512]; fmt_into(re, buf, sizeof buf);
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "%s:%d: RHS eval error: %s  -- src: %s",
                         path, line_no, buf, rhs);
                ray_release(le);          /* le is a value, not an error */
                ray_error_free(re);       /* re is the error */
                res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                goto done;
            }
            if (!fmt_eq(le, re)) {
                char lbuf[512], rbuf[512];
                fmt_into(le, lbuf, sizeof lbuf);
                fmt_into(re, rbuf, sizeof rbuf);
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "%s:%d: expected \"%s\", got \"%s\"  -- src: %s",
                         path, line_no, rbuf, lbuf, lhs);
                ray_release(le); ray_release(re);
                res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                goto done;
            }
            ray_release(le); ray_release(re);
        } else if (er) {
            assert_count++;
            *er = '\0';
            char* expr   = start;
            char* substr = er + 4;
            ray_t* ev = ray_eval_str(expr);
            if (!RAY_IS_ERR(ev)) {
                /* ev is a value here — we expected an error but got one. */
                char buf[512]; fmt_into(ev, buf, sizeof buf);
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "%s:%d: expected error containing \"%s\", got: %s  -- src: %s",
                         path, line_no, substr, buf, expr);
                if (ev) ray_release(ev);
                res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                goto done;
            }
            /* ev IS an error beyond this point — must use ray_error_free. */
            ray_t* es = ray_fmt(ev, 0);
            const char* ep = es ? ray_str_ptr(es) : "";
            if (!strstr(ep, substr)) {
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "%s:%d: error \"%s\" missing substr \"%s\"  -- src: %s",
                         path, line_no, ep, substr, expr);
                if (es) ray_release(es);
                ray_error_free(ev);
                res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                goto done;
            }
            if (es) ray_release(es);
            ray_error_free(ev);
        } else {
            /* Raw Rayfall code — eval; error is a test failure. */
            ray_t* ev = ray_eval_str(start);
            if (ev && RAY_IS_ERR(ev)) {
                char buf[512]; fmt_into(ev, buf, sizeof buf);
                snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                         "%s:%d: eval error: %s  -- src: %s",
                         path, line_no, buf, start);
                ray_error_free(ev);
                res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
                goto done;
            }
            if (ev) ray_release(ev);
        }

    next:
        if (nl_ptr) *nl_ptr = saved_nl;
        p = nl_ptr ? nl_ptr + 1 : p + line_len;
    }

done:
    /* Empty-coverage guard: a file with only comments or only raw setup
     * lines (no `--` / `!-` assertions) would otherwise report PASS with
     * zero effective checks — silent green.  Fail loudly so adding a new
     * .rfl file that forgot its assertions can't ship as "tested". */
    if (res.status == TEST_PASS && assert_count == 0) {
        snprintf(ray_test_fail_buf, sizeof ray_test_fail_buf,
                 "%s: no assertions found (needs at least one `--` or `!-` line)",
                 path);
        res = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }
    free(src);
    return res;
}

static test_result_t run_rfl_at(int idx) {
    if (idx < 0 || idx >= g_rfl_count) FAIL("invalid .rfl index");
    return run_rfl_file(g_rfl_paths[idx]);
}

static void rfl_setup(void)    { ray_runtime_create(0, NULL); }
static void rfl_teardown(void) { ray_runtime_destroy(__RUNTIME); }

/* Thunk pool — one function per potential .rfl slot. */
#define RFL_THUNKS(X) \
    X(  0) X(  1) X(  2) X(  3) X(  4) X(  5) X(  6) X(  7) \
    X(  8) X(  9) X( 10) X( 11) X( 12) X( 13) X( 14) X( 15) \
    X( 16) X( 17) X( 18) X( 19) X( 20) X( 21) X( 22) X( 23) \
    X( 24) X( 25) X( 26) X( 27) X( 28) X( 29) X( 30) X( 31) \
    X( 32) X( 33) X( 34) X( 35) X( 36) X( 37) X( 38) X( 39) \
    X( 40) X( 41) X( 42) X( 43) X( 44) X( 45) X( 46) X( 47) \
    X( 48) X( 49) X( 50) X( 51) X( 52) X( 53) X( 54) X( 55) \
    X( 56) X( 57) X( 58) X( 59) X( 60) X( 61) X( 62) X( 63) \
    X( 64) X( 65) X( 66) X( 67) X( 68) X( 69) X( 70) X( 71) \
    X( 72) X( 73) X( 74) X( 75) X( 76) X( 77) X( 78) X( 79) \
    X( 80) X( 81) X( 82) X( 83) X( 84) X( 85) X( 86) X( 87) \
    X( 88) X( 89) X( 90) X( 91) X( 92) X( 93) X( 94) X( 95) \
    X( 96) X( 97) X( 98) X( 99) X(100) X(101) X(102) X(103) \
    X(104) X(105) X(106) X(107) X(108) X(109) X(110) X(111) \
    X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) \
    X(120) X(121) X(122) X(123) X(124) X(125) X(126) X(127) \
    X(128) X(129) X(130) X(131) X(132) X(133) X(134) X(135) \
    X(136) X(137) X(138) X(139) X(140) X(141) X(142) X(143) \
    X(144) X(145) X(146) X(147) X(148) X(149) X(150) X(151) \
    X(152) X(153) X(154) X(155) X(156) X(157) X(158) X(159) \
    X(160) X(161) X(162) X(163) X(164) X(165) X(166) X(167) \
    X(168) X(169) X(170) X(171) X(172) X(173) X(174) X(175) \
    X(176) X(177) X(178) X(179) X(180) X(181) X(182) X(183) \
    X(184) X(185) X(186) X(187) X(188) X(189) X(190) X(191) \
    X(192) X(193) X(194) X(195) X(196) X(197) X(198) X(199) \
    X(200) X(201) X(202) X(203) X(204) X(205) X(206) X(207) \
    X(208) X(209) X(210) X(211) X(212) X(213) X(214) X(215) \
    X(216) X(217) X(218) X(219) X(220) X(221) X(222) X(223) \
    X(224) X(225) X(226) X(227) X(228) X(229) X(230) X(231) \
    X(232) X(233) X(234) X(235) X(236) X(237) X(238) X(239) \
    X(240) X(241) X(242) X(243) X(244) X(245) X(246) X(247) \
    X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255)

#define X(N) static test_result_t rfl_thunk_##N(void) { return run_rfl_at(N); }
RFL_THUNKS(X)
#undef X

#define X(N) rfl_thunk_##N,
static const test_func_t rfl_thunks[] = { RFL_THUNKS(X) };
#undef X

/* Walk `cur_dir` recursively.  `base_root` is the top-level directory
 * passed on the original call — kept constant across recursion so every
 * file's display name strips exactly that prefix, preserving category
 * structure (e.g. test/rfl/cmp/and.rfl → rfl/cmp/and).  Without this the
 * recursive call would re-root under the subdir and silently drop the
 * category segment from the test name. */
static int rfl_scan_at(const char* base_root, const char* cur_dir) {
    DIR* d = opendir(cur_dir);
    if (!d) return 0;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char full[512];
        snprintf(full, sizeof full, "%s/%s", cur_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (rfl_scan_at(base_root, full) < 0) { closedir(d); return -1; }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 4, ".rfl") != 0) continue;

        if (g_rfl_count >= RFL_THUNK_CAPACITY) {
            fprintf(stderr, "test driver: more than %d .rfl files — raise RFL_THUNK_CAPACITY\n",
                    RFL_THUNK_CAPACITY);
            closedir(d);
            return -1;
        }
        /* Copy full path into the path slot, bounds-checked. */
        {
            size_t flen = strlen(full);
            if (flen >= sizeof g_rfl_paths[0]) {
                fprintf(stderr, "test driver: .rfl path too long: %s\n", full);
                closedir(d); return -1;
            }
            memcpy(g_rfl_paths[g_rfl_count], full, flen + 1);
        }

        /* Name = path relative to BASE_ROOT (not cur_dir), ".rfl" stripped,
         * prefixed "rfl/".  This preserves every category segment.  Manual
         * bounds-check avoids -Werror=format-truncation on stricter GCCs. */
        const char* rel      = full;
        size_t      base_len = strlen(base_root);
        if (strncmp(full, base_root, base_len) == 0 && full[base_len] == '/')
            rel = full + base_len + 1;
        {
            char*  dst    = g_rfl_names[g_rfl_count];
            size_t cap    = sizeof g_rfl_names[0];     /* includes trailing NUL */
            size_t rellen = strlen(rel);
            if (rellen + 5 > cap) {                    /* 4 for "rfl/" + NUL   */
                fprintf(stderr, "test driver: .rfl test name too long: rfl/%s\n", rel);
                closedir(d); return -1;
            }
            memcpy(dst, "rfl/", 4);
            memcpy(dst + 4, rel, rellen + 1);
        }
        char* dot = strrchr(g_rfl_names[g_rfl_count], '.');
        if (dot && strcmp(dot, ".rfl") == 0) *dot = '\0';

        /* Duplicate-name guard: if two files ever resolve to the same test
         * name, munit will run both but a filter targets both at once — the
         * user can't isolate one.  Fail early so the layout can be fixed. */
        for (int i = 0; i < g_rfl_count; i++) {
            if (strcmp(g_rfl_names[i], g_rfl_names[g_rfl_count]) == 0) {
                fprintf(stderr,
                        "test driver: duplicate .rfl test name \"%s\":\n"
                        "    %s\n    %s\n",
                        g_rfl_names[g_rfl_count], g_rfl_paths[i], g_rfl_paths[g_rfl_count]);
                closedir(d);
                return -1;
            }
        }
        g_rfl_count++;
    }
    closedir(d);
    return 0;
}

/* Top-level entry.  Distinct from the recursive helper: a root that
 * doesn't exist or isn't a directory is FATAL (the previous behavior
 * silently skipped the entire .rfl suite on a bad root path, leaving
 * CI green with zero Rayfall coverage).  Subdir-level opendir failures
 * inside rfl_scan_at remain soft — a subdir that disappeared mid-walk
 * shouldn't abort an otherwise-valid scan. */
static int rfl_scan(const char* root_dir) {
    struct stat st;
    if (stat(root_dir, &st) != 0) {
        fprintf(stderr,
                "test driver: rfl root \"%s\" does not exist or cannot be stat'd.\n"
                "    Set RFL_ROOT to a valid directory, or run from a tree that has\n"
                "    test/rfl/.  Refusing to proceed — silent .rfl skip is not allowed.\n",
                root_dir);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        fprintf(stderr, "test driver: rfl root \"%s\" is not a directory.\n", root_dir);
        return -1;
    }
    if (rfl_scan_at(root_dir, root_dir) < 0) return -1;
    if (g_rfl_count == 0) {
        fprintf(stderr,
                "test driver: rfl root \"%s\" contains zero .rfl files.\n"
                "    Empty .rfl tree would ship the suite green with no Rayfall\n"
                "    coverage — treating as a fatal misconfiguration.\n",
                root_dir);
        return -1;
    }
    return 0;
}

/* ─── Runner ──────────────────────────────────────────────────────── */

static void print_status(test_status_t s, double ms, const char* msg) {
    switch (s) {
    case TEST_PASS:
        printf("%sPASS%s  %.2f ms\n", C_GREEN, C_RESET, ms);
        break;
    case TEST_SKIP:
        printf("%sSKIP%s  %s\n", C_YELLOW, C_RESET, msg ? msg : "");
        break;
    case TEST_FAIL:
        printf("%sFAIL%s\n        %s\n", C_RED, C_RESET, msg ? msg : "(no message)");
        break;
    }
}

static int run_one(const test_entry_t* e, int* pass, int* fail, int* skip) {
    printf("  %-52s  ", e->name);
    fflush(stdout);

    if (e->setup) e->setup();

    clock_t t0 = clock();
    test_result_t r;

    /* setjmp escape: any scalar-returning helper that hits an unrecoverable
     * error calls ray_test_fatal() which longjmp's back here.  The runner
     * reports the test as FAIL with the message helpers wrote into
     * ray_test_fail_buf, then continues to the next test. */
    if (setjmp(ray_test_jmp) == 0) {
        ray_test_jmp_active = 1;
        r = e->func();
        ray_test_jmp_active = 0;
    } else {
        ray_test_jmp_active = 0;
        r = (test_result_t){ TEST_FAIL, ray_test_fail_buf };
    }

    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;

    if (e->teardown) e->teardown();

    print_status(r.status, ms, r.msg);

    if      (r.status == TEST_PASS) (*pass)++;
    else if (r.status == TEST_SKIP) (*skip)++;
    else                            (*fail)++;
    return r.status == TEST_FAIL ? 1 : 0;
}

static int name_matches_filter(const char* name, const char* filter) {
    if (!filter || !*filter) return 1;
    return strstr(name, filter) != NULL;
}

int main(int argc, char** argv) {
    g_color = isatty(fileno(stdout));

    const char* filter = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--filter") == 0 || strcmp(argv[i], "-f") == 0)
            && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-f SUBSTR]\n", argv[0]);
            printf("  -f, --filter SUBSTR   Only run tests whose name contains SUBSTR.\n");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    const char* rfl_root = getenv("RFL_ROOT");
    if (!rfl_root || !*rfl_root) rfl_root = "test/rfl";
    if (rfl_scan(rfl_root) < 0) return 2;

    int pass = 0, fail = 0, skip = 0, total = 0;

    /* Compiled-in groups */
    for (int gi = 0; compiled_groups[gi]; gi++) {
        const test_entry_t* g = compiled_groups[gi];
        for (int i = 0; g[i].name; i++) {
            if (!name_matches_filter(g[i].name, filter)) continue;
            total++;
            run_one(&g[i], &pass, &fail, &skip);
        }
    }

    /* Dynamic .rfl group */
    for (int i = 0; i < g_rfl_count; i++) {
        if (!name_matches_filter(g_rfl_names[i], filter)) continue;
        total++;
        test_entry_t e = {
            .name     = g_rfl_names[i],
            .func     = rfl_thunks[i],
            .setup    = rfl_setup,
            .teardown = rfl_teardown,
        };
        run_one(&e, &pass, &fail, &skip);
    }

    printf("\n%s=== %d of %d passed (%d skipped, %d failed) ===%s\n",
           fail ? C_RED : (skip ? C_YELLOW : C_GREEN),
           pass, total, skip, fail, C_RESET);

    return fail ? 1 : 0;
}
