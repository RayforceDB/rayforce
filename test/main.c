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
#include "ops/internal.h"
#include "ops/idxop.h"

/* __RUNTIME is internal test plumbing; runtime API declarations come from
 * <rayforce.h>. */
extern ray_runtime_t* __RUNTIME;

/* Runtime poll accessors (core/runtime.h) + poll lifecycle (core/poll.h),
 * forward-declared to avoid the ray_vm_t clash between core/runtime.h and
 * lang/eval.h (included above). */
struct ray_poll;
typedef struct ray_poll ray_poll_t;
extern ray_poll_t* ray_poll_create(void);
extern void        ray_poll_destroy(ray_poll_t* poll);
extern void        ray_runtime_set_poll(void* poll);
extern void*       ray_runtime_get_poll(void);

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
extern const test_entry_t aof_entries[];
extern const test_entry_t arena_entries[];
extern const test_entry_t atom_entries[];
extern const test_entry_t audit_entries[];
extern const test_entry_t block_entries[];
extern const test_entry_t buddy_entries[];
extern const test_entry_t compile_entries[];
extern const test_entry_t cow_entries[];
extern const test_entry_t csr_entries[];
extern const test_entry_t csv_entries[];
extern const test_entry_t datalog_entries[];
extern const test_entry_t dict_entries[];
extern const test_entry_t domain_entries[];
extern const test_entry_t dump_entries[];
extern const test_entry_t embedding_entries[];
extern const test_entry_t exec_entries[];
extern const test_entry_t expr_null_entries[];
extern const test_entry_t f64_nullmodel_entries[];
extern const test_entry_t format_entries[];
extern const test_entry_t fvec_entries[];
extern const test_entry_t graph_entries[];
extern const test_entry_t agg_registry_entries[];
extern const test_entry_t agg_engine_entries[];
extern const test_entry_t agg_contract_entries[];
extern const test_entry_t graph_builtin_entries[];
extern const test_entry_t group_extra_entries[];
extern const test_entry_t group_pushdown_entries[];
extern const test_entry_t fused_topk_entries[];
extern const test_entry_t hash_entries[];
extern const test_entry_t heap_entries[];
extern const test_entry_t heap_parallel_entries[];
extern const test_entry_t idx_route_entries[];
extern const test_entry_t index_entries[];
extern const test_entry_t ipc_entries[];
extern const test_entry_t join_buildside_entries[];
extern const test_entry_t journal_entries[];
extern const test_entry_t lang_entries[];
extern const test_entry_t link_entries[];
extern const test_entry_t lftj_entries[];
extern const test_entry_t list_entries[];
extern const test_entry_t meta_entries[];
extern const test_entry_t morsel_entries[];
extern const test_entry_t mcast_entries[];
extern const test_entry_t numparse_entries[];
extern const test_entry_t opt_entries[];
extern const test_entry_t partition_exec_entries[];
extern const test_entry_t pipe_entries[];
extern const test_entry_t platform_entries[];
extern const test_entry_t pool_entries[];
extern const test_entry_t progress_entries[];
extern const test_entry_t public_api_entries[];
extern const test_entry_t repl_entries[];
extern const test_entry_t rowsel_entries[];
extern const test_entry_t runtime_entries[];
extern const test_entry_t sel_entries[];
extern const test_entry_t sort_entries[];
extern const test_entry_t splay_entries[];
extern const test_entry_t store_entries[];
extern const test_entry_t stress_eval_entries[];
extern const test_entry_t stress_matrix_entries[];
extern const test_entry_t stress_random_entries[];
extern const test_entry_t str_entries[];
extern const test_entry_t sym_entries[];
extern const test_entry_t text_null_entries[];
extern const test_entry_t sys_entries[];
extern const test_entry_t table_entries[];
extern const test_entry_t term_entries[];
extern const test_entry_t traverse_entries[];
extern const test_entry_t types_entries[];
extern const test_entry_t vec_entries[];
extern const test_entry_t window_entries[];

static const test_entry_t* const compiled_groups[] = {
    err_entries,      aof_entries,      arena_entries,    atom_entries,     audit_entries,
    block_entries,    buddy_entries,    compile_entries,  cow_entries,      csr_entries,
    csv_entries,      datalog_entries,  dict_entries,     domain_entries,
    dump_entries,
    embedding_entries, exec_entries,   expr_null_entries,
    f64_nullmodel_entries,
    format_entries,   fvec_entries,     graph_entries,    graph_builtin_entries,
    agg_registry_entries,
    agg_engine_entries,
    agg_contract_entries,
    group_extra_entries,
    group_pushdown_entries,
    fused_topk_entries,
    hash_entries,
    heap_entries,
    heap_parallel_entries,
    idx_route_entries,
    index_entries,    ipc_entries,
    join_buildside_entries,
    journal_entries,
    lang_entries,     link_entries,
    lftj_entries,     list_entries,     meta_entries,     morsel_entries,
    mcast_entries,
    numparse_entries, opt_entries,      partition_exec_entries,
    pipe_entries,     platform_entries,
    pool_entries,     progress_entries,
    public_api_entries,
    repl_entries,     rowsel_entries,   runtime_entries,  sel_entries,
    sort_entries,     splay_entries,    store_entries,
    stress_eval_entries,
    stress_matrix_entries,
    stress_random_entries,
    text_null_entries, str_entries,      sym_entries,      sys_entries,      table_entries,
    term_entries,     traverse_entries,
    types_entries,    vec_entries,      window_entries,
    NULL,
};

/* ─── .rfl auto-discovery ─────────────────────────────────────────── */
/*
 * A pool of pre-declared thunks dispatches loaded .rfl files by index.
 * Each thunk calls run_rfl_at(N) which loads the file at that slot and
 * evaluates it under a fresh runtime (via rfl_setup/rfl_teardown).
 */

#define RFL_THUNK_CAPACITY 1024

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
    /* ray_eval_str returns RAY_NULL_OBJ for successful void results (like
     * evaluating `null`). Bare C NULL is reserved for defensive no-result
     * handling: two NULLs compare equal; one differs from anything else. */
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

/* RFL_UPDATE=1 regen: rewrite a .rfl's ` -- RHS` goldens with actual values. */
static void rfl_rewrite_goldens(const char* path, const int* lns, char (*vals)[512], int nu) {
    FILE* f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char* s = (char*)malloc((size_t)n + 1); if (!s) { fclose(f); return; }
    size_t rd = fread(s, 1, (size_t)n, f); s[rd] = '\0'; fclose(f);
    FILE* o = fopen(path, "wb"); if (!o) { free(s); return; }
    char* q = s; int ln = 0;
    while (*q) {
        char* nl = strchr(q, '\n'); size_t L = nl ? (size_t)(nl - q) : strlen(q);
        ln++;
        int ui = -1; for (int i = 0; i < nu; i++) if (lns[i] == ln) { ui = i; break; }
        if (ui >= 0) {
            char line[2048]; size_t cl = L < 2047 ? L : 2047; memcpy(line, q, cl); line[cl] = '\0';
            char* sep = find_top_sep(line, " -- ");
            if (sep) { *(sep + 4) = '\0'; fprintf(o, "%s%s", line, vals[ui]); }
            else fwrite(q, 1, L, o);
        } else fwrite(q, 1, L, o);
        if (nl) fputc('\n', o);
        q = nl ? nl + 1 : q + L;
    }
    free(s); fclose(o);
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
    int   upd    = getenv("RFL_UPDATE") != NULL;
    static int  upd_ln[4096];
    static char upd_v[4096][512];
    int   nu     = 0;

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
                if (upd) {
                    if (nu < 4096) { upd_ln[nu] = line_no;
                        snprintf(upd_v[nu], sizeof upd_v[nu], "%s", lbuf); nu++; }
                    ray_release(le); ray_release(re);
                    goto next;
                }
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
    if (upd && nu) rfl_rewrite_goldens(path, upd_ln, upd_v, nu);
    free(src);
    return res;
}

static test_result_t run_rfl_at(int idx) {
    if (idx < 0 || idx >= g_rfl_count) FAIL("invalid .rfl index");
    return run_rfl_file(g_rfl_paths[idx]);
}

/* The runtime poll mirrors main.c: unified IPC handles are selector ids
 * resolved in it, so .rfl scripts that `.ipc.open` need one published.
 * Destroy order also mirrors main.c — poll first, runtime second. */
static void rfl_setup(void) {
    ray_runtime_create(0, NULL);
    ray_poll_t* p = ray_poll_create();
    if (p) ray_runtime_set_poll(p);
}
static void rfl_teardown(void) {
    ray_poll_t* p = (ray_poll_t*)ray_runtime_get_poll();
    if (p) {
        ray_runtime_set_poll(NULL);
        ray_poll_destroy(p);
    }
    ray_runtime_destroy(__RUNTIME);
}

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
    X(248) X(249) X(250) X(251) X(252) X(253) X(254) X(255) \
    X(256) X(257) X(258) X(259) X(260) X(261) X(262) X(263) \
    X(264) X(265) X(266) X(267) X(268) X(269) X(270) X(271) \
    X(272) X(273) X(274) X(275) X(276) X(277) X(278) X(279) \
    X(280) X(281) X(282) X(283) X(284) X(285) X(286) X(287) \
    X(288) X(289) X(290) X(291) X(292) X(293) X(294) X(295) \
    X(296) X(297) X(298) X(299) X(300) X(301) X(302) X(303) \
    X(304) X(305) X(306) X(307) X(308) X(309) X(310) X(311) \
    X(312) X(313) X(314) X(315) X(316) X(317) X(318) X(319) \
    X(320) X(321) X(322) X(323) X(324) X(325) X(326) X(327) \
    X(328) X(329) X(330) X(331) X(332) X(333) X(334) X(335) \
    X(336) X(337) X(338) X(339) X(340) X(341) X(342) X(343) \
    X(344) X(345) X(346) X(347) X(348) X(349) X(350) X(351) \
    X(352) X(353) X(354) X(355) X(356) X(357) X(358) X(359) \
    X(360) X(361) X(362) X(363) X(364) X(365) X(366) X(367) \
    X(368) X(369) X(370) X(371) X(372) X(373) X(374) X(375) \
    X(376) X(377) X(378) X(379) X(380) X(381) X(382) X(383) \
    X(384) X(385) X(386) X(387) X(388) X(389) X(390) X(391) \
    X(392) X(393) X(394) X(395) X(396) X(397) X(398) X(399) \
    X(400) X(401) X(402) X(403) X(404) X(405) X(406) X(407) \
    X(408) X(409) X(410) X(411) X(412) X(413) X(414) X(415) \
    X(416) X(417) X(418) X(419) X(420) X(421) X(422) X(423) \
    X(424) X(425) X(426) X(427) X(428) X(429) X(430) X(431) \
    X(432) X(433) X(434) X(435) X(436) X(437) X(438) X(439) \
    X(440) X(441) X(442) X(443) X(444) X(445) X(446) X(447) \
    X(448) X(449) X(450) X(451) X(452) X(453) X(454) X(455) \
    X(456) X(457) X(458) X(459) X(460) X(461) X(462) X(463) \
    X(464) X(465) X(466) X(467) X(468) X(469) X(470) X(471) \
    X(472) X(473) X(474) X(475) X(476) X(477) X(478) X(479) \
    X(480) X(481) X(482) X(483) X(484) X(485) X(486) X(487) \
    X(488) X(489) X(490) X(491) X(492) X(493) X(494) X(495) \
    X(496) X(497) X(498) X(499) X(500) X(501) X(502) X(503) \
    X(504) X(505) X(506) X(507) X(508) X(509) X(510) X(511) \
    X(512) X(513) X(514) X(515) X(516) X(517) X(518) X(519) \
    X(520) X(521) X(522) X(523) X(524) X(525) X(526) X(527) \
    X(528) X(529) X(530) X(531) X(532) X(533) X(534) X(535) \
    X(536) X(537) X(538) X(539) X(540) X(541) X(542) X(543) \
    X(544) X(545) X(546) X(547) X(548) X(549) X(550) X(551) \
    X(552) X(553) X(554) X(555) X(556) X(557) X(558) X(559) \
    X(560) X(561) X(562) X(563) X(564) X(565) X(566) X(567) \
    X(568) X(569) X(570) X(571) X(572) X(573) X(574) X(575) \
    X(576) X(577) X(578) X(579) X(580) X(581) X(582) X(583) \
    X(584) X(585) X(586) X(587) X(588) X(589) X(590) X(591) \
    X(592) X(593) X(594) X(595) X(596) X(597) X(598) X(599) \
    X(600) X(601) X(602) X(603) X(604) X(605) X(606) X(607) \
    X(608) X(609) X(610) X(611) X(612) X(613) X(614) X(615) \
    X(616) X(617) X(618) X(619) X(620) X(621) X(622) X(623) \
    X(624) X(625) X(626) X(627) X(628) X(629) X(630) X(631) \
    X(632) X(633) X(634) X(635) X(636) X(637) X(638) X(639) \
    X(640) X(641) X(642) X(643) X(644) X(645) X(646) X(647) \
    X(648) X(649) X(650) X(651) X(652) X(653) X(654) X(655) \
    X(656) X(657) X(658) X(659) X(660) X(661) X(662) X(663) \
    X(664) X(665) X(666) X(667) X(668) X(669) X(670) X(671) \
    X(672) X(673) X(674) X(675) X(676) X(677) X(678) X(679) \
    X(680) X(681) X(682) X(683) X(684) X(685) X(686) X(687) \
    X(688) X(689) X(690) X(691) X(692) X(693) X(694) X(695) \
    X(696) X(697) X(698) X(699) X(700) X(701) X(702) X(703) \
    X(704) X(705) X(706) X(707) X(708) X(709) X(710) X(711) \
    X(712) X(713) X(714) X(715) X(716) X(717) X(718) X(719) \
    X(720) X(721) X(722) X(723) X(724) X(725) X(726) X(727) \
    X(728) X(729) X(730) X(731) X(732) X(733) X(734) X(735) \
    X(736) X(737) X(738) X(739) X(740) X(741) X(742) X(743) \
    X(744) X(745) X(746) X(747) X(748) X(749) X(750) X(751) \
    X(752) X(753) X(754) X(755) X(756) X(757) X(758) X(759) \
    X(760) X(761) X(762) X(763) X(764) X(765) X(766) X(767) \
    X(768) X(769) X(770) X(771) X(772) X(773) X(774) X(775) \
    X(776) X(777) X(778) X(779) X(780) X(781) X(782) X(783) \
    X(784) X(785) X(786) X(787) X(788) X(789) X(790) X(791) \
    X(792) X(793) X(794) X(795) X(796) X(797) X(798) X(799) \
    X(800) X(801) X(802) X(803) X(804) X(805) X(806) X(807) \
    X(808) X(809) X(810) X(811) X(812) X(813) X(814) X(815) \
    X(816) X(817) X(818) X(819) X(820) X(821) X(822) X(823) \
    X(824) X(825) X(826) X(827) X(828) X(829) X(830) X(831) \
    X(832) X(833) X(834) X(835) X(836) X(837) X(838) X(839) \
    X(840) X(841) X(842) X(843) X(844) X(845) X(846) X(847) \
    X(848) X(849) X(850) X(851) X(852) X(853) X(854) X(855) \
    X(856) X(857) X(858) X(859) X(860) X(861) X(862) X(863) \
    X(864) X(865) X(866) X(867) X(868) X(869) X(870) X(871) \
    X(872) X(873) X(874) X(875) X(876) X(877) X(878) X(879) \
    X(880) X(881) X(882) X(883) X(884) X(885) X(886) X(887) \
    X(888) X(889) X(890) X(891) X(892) X(893) X(894) X(895) \
    X(896) X(897) X(898) X(899) X(900) X(901) X(902) X(903) \
    X(904) X(905) X(906) X(907) X(908) X(909) X(910) X(911) \
    X(912) X(913) X(914) X(915) X(916) X(917) X(918) X(919) \
    X(920) X(921) X(922) X(923) X(924) X(925) X(926) X(927) \
    X(928) X(929) X(930) X(931) X(932) X(933) X(934) X(935) \
    X(936) X(937) X(938) X(939) X(940) X(941) X(942) X(943) \
    X(944) X(945) X(946) X(947) X(948) X(949) X(950) X(951) \
    X(952) X(953) X(954) X(955) X(956) X(957) X(958) X(959) \
    X(960) X(961) X(962) X(963) X(964) X(965) X(966) X(967) \
    X(968) X(969) X(970) X(971) X(972) X(973) X(974) X(975) \
    X(976) X(977) X(978) X(979) X(980) X(981) X(982) X(983) \
    X(984) X(985) X(986) X(987) X(988) X(989) X(990) X(991) \
    X(992) X(993) X(994) X(995) X(996) X(997) X(998) X(999) \
    X(1000) X(1001) X(1002) X(1003) X(1004) X(1005) X(1006) X(1007) \
    X(1008) X(1009) X(1010) X(1011) X(1012) X(1013) X(1014) X(1015) \
    X(1016) X(1017) X(1018) X(1019) X(1020) X(1021) X(1022) X(1023)

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

        _Static_assert(sizeof(rfl_thunks) / sizeof(*rfl_thunks) == RFL_THUNK_CAPACITY,
                       "one thunk per .rfl slot: extend RFL_THUNKS with RFL_THUNK_CAPACITY");
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
    ray_expr_stats_init();
    ray_idx_stats_init();
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
