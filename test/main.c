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
#include "ops/agg_engine.h"   /* --census: agg_route_stats */
#include "core/pool.h"         /* ray_pool_test_ring_fail */
#include "core/platform.h"     /* ray_file_writeback_test_hold */
#include "mem/heap.h"          /* ray_alloc_fail_set_for_test */
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
#include "store/fileio.h"   /* ray_mkdir_p — ray_test_mkdir_p */
#include "store/col.h"      /* ray_store_test_force_compress — equivalence pass */
#include "store/enccol.h"   /* ray_test_hooks_off */
#include "table/domain.h"   /* ray_sym_domain_test_hook */
#include "mem/heap.h"       /* ray_heap_test_scribble */
#include "store/encw.h"
#include "ops/colbind.h"

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
extern const test_entry_t parquet_entries[];
extern const test_entry_t datalog_entries[];
extern const test_entry_t dict_entries[];
extern const test_entry_t domain_entries[];
extern const test_entry_t dump_entries[];
extern const test_entry_t embedding_entries[];
extern const test_entry_t exec_entries[];
extern const test_entry_t expr_null_entries[];
extern const test_entry_t null_flag_entries[];
extern const test_entry_t window_sum_entries[];
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
extern const test_entry_t join_mask_entries[];
extern const test_entry_t join_prefilter_entries[];
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
extern const test_entry_t shm_entries[];
extern const test_entry_t rdma_entries[];
extern const test_entry_t public_api_entries[];
extern const test_entry_t repl_entries[];
extern const test_entry_t rowsel_entries[];
extern const test_entry_t runtime_entries[];
extern const test_entry_t sel_entries[];
extern const test_entry_t select_merge_entries[];
extern const test_entry_t sort_entries[];
extern const test_entry_t splay_entries[];
extern const test_entry_t srccheck_entries[];
extern const test_entry_t builtin_names_entries[];
extern const test_entry_t store_entries[];
extern const test_entry_t idx_uses_entries[];
extern const test_entry_t enc_entries[];
extern const test_entry_t encfmt_entries[];
extern const test_entry_t encidx_entries[];
extern const test_entry_t encfloat_entries[];
extern const test_entry_t encstr_entries[];
extern const test_entry_t enconce_entries[];
extern const test_entry_t enccol_entries[];
extern const test_entry_t large_entries[];
extern const test_entry_t colrange_entries[];
extern const test_entry_t scanrange_entries[];
extern const test_entry_t aware_entries[];
extern const test_entry_t parted_ragged_entries[];
extern const test_entry_t compress_entries[];
extern const test_entry_t fsst_entries[];
extern const test_entry_t stress_eval_entries[];
extern const test_entry_t stress_matrix_entries[];
extern const test_entry_t stress_random_entries[];
extern const test_entry_t str_entries[];
extern const test_entry_t sym_entries[];
extern const test_entry_t read_budget_entries[];
extern const test_entry_t readval_entries[];
extern const test_entry_t sym_publish_entries[];
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
    parquet_entries, csv_entries,      datalog_entries,  dict_entries,     domain_entries,
    dump_entries,
    embedding_entries, exec_entries,   expr_null_entries,
    null_flag_entries,
    window_sum_entries,
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
    join_mask_entries,
    join_prefilter_entries,
    journal_entries,
    lang_entries,     link_entries,
    lftj_entries,     list_entries,     meta_entries,     morsel_entries,
    mcast_entries,
    numparse_entries, opt_entries,      partition_exec_entries,
    pipe_entries,     platform_entries,
    pool_entries,     progress_entries, shm_entries,      rdma_entries,
    public_api_entries,
    repl_entries,     rowsel_entries,   runtime_entries,  sel_entries, select_merge_entries,
    sort_entries,     splay_entries,    srccheck_entries, builtin_names_entries, store_entries,    enc_entries,
    encfmt_entries,   encidx_entries,   encfloat_entries, encstr_entries, enconce_entries,   enccol_entries,
    large_entries,
    colrange_entries,
    scanrange_entries,
    aware_entries,
    parted_ragged_entries,
    compress_entries,
    idx_uses_entries,
    fsst_entries,
    stress_eval_entries,
    stress_matrix_entries,
    stress_random_entries,
    read_budget_entries, readval_entries, sym_publish_entries,
    text_null_entries, str_entries,      sym_entries,      sys_entries,      table_entries,
    term_entries,     traverse_entries,
    types_entries,    vec_entries,      window_entries,
    NULL,
};

/* ─── .rfl auto-discovery ─────────────────────────────────────────── */
/*
 * Every .rfl file under the root is a slot of g_rfl, which grows as the
 * scan finds them (no fixed count).  Its jobs all run one function,
 * rfl_thunk, which loads the file of the job's slot (test_job_t.rfl, set
 * for the job by run_job in g_rfl_cur) and evaluates it under a fresh
 * runtime (via rfl_setup/rfl_teardown).
 */

typedef struct { char path[512]; char name[256]; } rfl_slot_t;
static rfl_slot_t* g_rfl;
static int         g_rfl_count = 0, g_rfl_cap = 0;
static int         g_rfl_cur = -1;   /* the running job's slot */

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

/* --census PATH: after every evaluated .rfl line, record grouped selects
 * that reached the legacy grouping ladder (route counters are per thread
 * and reset before each line).  One tab-separated line per hit:
 * reason, file:line, source.  Diagnostic only; off unless the flag is set. */
static FILE* g_census = NULL;
static const char* census_reason_name(agg_v2_reason_t r) {
    switch (r) {
        case AGG_V2_ADMITTED:       return "admitted";
        case AGG_V2_SHAPE:          return "shape";
        case AGG_V2_KEY_EXPRESSION: return "key_expression";
        case AGG_V2_KEY_TYPE:       return "key_type";
        case AGG_V2_AGG_EXPRESSION: return "agg_expression";
        case AGG_V2_AGG_TYPE:       return "agg_type";
        case AGG_V2_BUFFERED:       return "buffered";
        case AGG_V2_PARAMETER:      return "parameter";
        case AGG_V2_DISABLED:       return "disabled";
        case AGG_V2_EMIT_FILTER:    return "emit_filter";
        case AGG_V2_PARALLEL_WIDE:  return "parallel_wide";
    }
    return "unknown";
}
static ray_t* rfl_eval(const char* src, const char* path, int line_no) {
    if (g_census) agg_route_reset();
    ray_t* v = ray_eval_str(src);
    if (g_census) {
        agg_route_stats_t st = agg_route_stats();
        if (st.routes[AGG_ROUTE_LEGACY] > 0)
            fprintf(g_census, "%s\t%s:%d\t%s\n", census_reason_name(st.last_v2_reason), path, line_no, src);
    }
    return v;
}

static char* tmp_subst(const char* s);   /* the per-process /tmp rewrite, below */

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
    /* every "/tmp" path the file names moves under this process's root */
    char* moved = tmp_subst(src);
    if (moved) { free(src); src = moved; }

    /* ";; @requires: posix" anywhere in a file marks it as depending on a
     * POSIX shell / filesystem (.sys.exec pipelines, /proc).  Where that
     * does not exist the file is reported as SKIP, never silently dropped. */
#if defined(_WIN32)
    if (strstr(src, ";; @requires: posix")) {
        free(src);
        SKIP("requires POSIX shell/filesystem");
    }
#endif
    /* ";; @requires: linux": Linux-only facilities (taskset, cgroups). */
#if !defined(__linux__)
    if (strstr(src, ";; @requires: linux")) {
        free(src);
        SKIP("requires Linux");
    }
#endif

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
            ray_t* le = rfl_eval(lhs, path, line_no);
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
            ray_t* ev = rfl_eval(expr, path, line_no);
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
            ray_t* ev = rfl_eval(start, path, line_no);
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

/* ";; @also-unfused" in a file runs it a second time with the fused
 * expression evaluator turned off, so every query also goes through the
 * typed per-node executor (the path a fused program falls back to when it
 * runs out of registers) and is held to the same answers. */
extern bool ray_expr_disable;
static bool rfl_file_has(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (!found && fgets(line, sizeof line, f)) found = strstr(line, needle) != NULL;
    fclose(f);
    return found;
}

static test_result_t run_rfl_at(int idx) {
    if (idx < 0 || idx >= g_rfl_count) FAIL("invalid .rfl index");
    test_result_t r = run_rfl_file(g_rfl[idx].path);
    if (r.status == TEST_PASS && rfl_file_has(g_rfl[idx].path, ";; @also-unfused")) {
        ray_expr_disable = true;
        r = run_rfl_file(g_rfl[idx].path);
        ray_expr_disable = false;
        if (r.status == TEST_FAIL) fprintf(stderr, "        (in the unfused pass)\n");
    }
    return r;
}

/* The runtime poll mirrors main.c: unified IPC handles are selector ids
 * resolved in it, so .rfl scripts that `.ipc.open` need one published.
 * Destroy order also mirrors main.c — poll first, runtime second. */
static void rfl_setup(void) {
    ray_runtime_create(0, NULL);
    /* peak-bytes asserts measure this file, not what ran before it */
    ray_mem_peak_reset_for_test();
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

/* Every .rfl job's function: the file of the job's slot. */
static test_result_t rfl_thunk(void) { return run_rfl_at(g_rfl_cur); }

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
        if (stat(full, &st) != 0) {   /* e.g. a dangling link: not silently left out */
            fprintf(stderr, "test driver: cannot stat %s under the rfl root\n", full);
            closedir(d);
            return -1;
        }
        if (S_ISDIR(st.st_mode)) {
            if (rfl_scan_at(base_root, full) < 0) { closedir(d); return -1; }
            continue;
        }
        if (!S_ISREG(st.st_mode)) continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 4, ".rfl") != 0) continue;

        if (g_rfl_count == g_rfl_cap) {
            int cap = g_rfl_cap ? 2 * g_rfl_cap : 1024;
            rfl_slot_t* grown = (rfl_slot_t*)realloc(g_rfl, (size_t)cap * sizeof *g_rfl);
            if (!grown) {
                fprintf(stderr, "test driver: out of memory for %d .rfl files\n", cap);
                closedir(d);
                return -1;
            }
            g_rfl = grown; g_rfl_cap = cap;
        }
        /* Copy full path into the path slot, bounds-checked. */
        {
            size_t flen = strlen(full);
            if (flen >= sizeof g_rfl[0].path) {
                fprintf(stderr, "test driver: .rfl path too long: %s\n", full);
                closedir(d); return -1;
            }
            memcpy(g_rfl[g_rfl_count].path, full, flen + 1);
        }

        /* Name = path relative to BASE_ROOT (not cur_dir), ".rfl" stripped,
         * prefixed "rfl/".  This preserves every category segment.  Manual
         * bounds-check avoids -Werror=format-truncation on stricter GCCs. */
        const char* rel      = full;
        size_t      base_len = strlen(base_root);
        if (strncmp(full, base_root, base_len) == 0 && full[base_len] == '/')
            rel = full + base_len + 1;
        {
            char*  dst    = g_rfl[g_rfl_count].name;
            size_t cap    = sizeof g_rfl[0].name;     /* includes trailing NUL */
            size_t rellen = strlen(rel);
            if (rellen + 5 > cap) {                    /* 4 for "rfl/" + NUL   */
                fprintf(stderr, "test driver: .rfl test name too long: rfl/%s\n", rel);
                closedir(d); return -1;
            }
            memcpy(dst, "rfl/", 4);
            memcpy(dst + 4, rel, rellen + 1);
        }
        char* dot = strrchr(g_rfl[g_rfl_count].name, '.');
        if (dot && strcmp(dot, ".rfl") == 0) *dot = '\0';

        /* Duplicate-name guard: if two files ever resolve to the same test
         * name, munit will run both but a filter targets both at once — the
         * user can't isolate one.  Fail early so the layout can be fixed. */
        for (int i = 0; i < g_rfl_count; i++) {
            if (strcmp(g_rfl[i].name, g_rfl[g_rfl_count].name) == 0) {
                fprintf(stderr,
                        "test driver: duplicate .rfl test name \"%s\":\n"
                        "    %s\n    %s\n",
                        g_rfl[g_rfl_count].name, g_rfl[i].path, g_rfl[g_rfl_count].path);
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

#include "env_restore.h"   /* run_one: env_snapshot / env_restore */

/* The test body under the setjmp escape: any scalar-returning helper that
 * hits an unrecoverable error calls ray_test_fatal(), which longjmp's back
 * here.  The runner reports the test as FAIL with the message helpers wrote
 * into ray_test_fail_buf, then continues to the next test.
 *
 * Its own function so that nothing run_one keeps across the test (the
 * environment snapshot, the start time) lives in the frame setjmp returns
 * into: a local of that frame is indeterminate after the longjmp unless it
 * is volatile (C11 7.13.2.1), and gcc -Wclobbered warns on any it cannot
 * prove safe.  A function that calls setjmp is never inlined. */
static test_result_t run_guarded(const test_entry_t* e) {
    if (setjmp(ray_test_jmp) == 0) {
        ray_test_jmp_active = 1;
        test_result_t r = e->func();
        ray_test_jmp_active = 0;
        return r;
    }
    ray_test_jmp_active = 0;
    return (test_result_t){ TEST_FAIL, ray_test_fail_buf };
}

static int run_one(const test_entry_t* e, int* pass, int* fail, int* skip) {
    printf("  %-52s  ", e->name);
    fflush(stdout);

    char** env = env_snapshot();
    if (e->setup) e->setup();

    clock_t t0 = clock();
    test_result_t r = run_guarded(e);

    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;

    if (e->teardown) e->teardown();
    env_restore(env);

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

/* ---- Shell-free filesystem helpers (declared in test.h) ---- */

bool ray_test_nulls_agree(ray_t* v, char* why, size_t cap) {
    if (!v || RAY_IS_ERR(v) || !ray_is_vec(v)) return true;
    switch (v->type) {
        case RAY_F64: case RAY_F32:
        case RAY_I64: case RAY_TIMESPAN: case RAY_TIMESTAMP:
        case RAY_I32: case RAY_DATE: case RAY_TIME:
        case RAY_I16: case RAY_GUID:
            break;
        default:
            return true;
    }
    /* A slice has no flag of its own: readers consult the parent's.
     * ray_data resolves the slice's window into the parent's payload. */
    const ray_t* base = (v->attrs & RAY_ATTR_SLICE) ? v->slice_parent : v;
    bool flag = (base->attrs & RAY_ATTR_HAS_NULLS) != 0;
    const void* data = ray_data(v);
    int64_t first = -1, count = 0;
    for (int64_t i = 0; i < v->len; i++) {
        if (ray_raw_is_null(v->type, base->attrs, data, i)) {
            if (first < 0) first = i;
            count++;
        }
    }
    if (!flag && count > 0) {
        snprintf(why, cap, "%s vector of %lld has %lld null sentinel(s), first at %lld, "
                 "but HAS_NULLS is clear", ray_type_name(v->type),
                 (long long)v->len, (long long)count, (long long)first);
        return false;
    }
    if (flag && count == 0 && !(v->attrs & RAY_ATTR_SLICE)) {
        snprintf(why, cap, "%s vector of %lld has HAS_NULLS set but no null sentinel",
                 ray_type_name(v->type), (long long)v->len);
        return false;
    }
    return true;
}

int ray_test_rm_rf(const char* path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;          /* already gone */
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path);
        if (d) {
            struct dirent* ent;
            char child[4096];
            while ((ent = readdir(d)) != NULL) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                    continue;
                snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
                ray_test_rm_rf(child);
            }
            closedir(d);
        }
        return rmdir(path);
    }
    return unlink(path);
}

int ray_test_mkdir_p(const char* path) {
    return ray_mkdir_p(path) == RAY_OK ? 0 : -1;
}

void ray_test_hooks_off(void) {
    ray_enccol_test_fail_alloc(0);
    ray_enccol_test_fail_path(NULL);
    ray_enccol_test_interrupt_after(0);
    ray_enccol_test_hold_decode(false);
    ray_enccol_test_parted_mode(0);
    ray_enccol_test_fail_index_parse(false);
    ray_enccol_test_kept_limit(0);
    ray_enccol_test_cache_drop_all();     /* before the teardown's ray_heap_destroy */
    ray_col_test_fail_alloc(0);
    ray_col_test_no_dict(false);
    ray_encw_test_force(-1);
    ray_encw_test_force_float(-1);
    ray_encw_test_force_str(-1);
    ray_graph_bind_test_fail_alloc(0);
    ray_store_test_force_compress(false);
    ray_idx_lazy_test_fail_build(NULL);
    ray_expr_test_parted_all_segments(false);
    ray_group_test_route(0);
    ray_query_test_cd_route(0);
    ray_query_test_cancel_net_off(false);
    ray_query_test_cd_firstn_set_max(-1);
    ray_group_test_fail_scalar_alloc(0);
    ray_group_test_scalar_grain(0);
    ray_in_guid_test_fail_alloc(0);
    ray_distinct_guid_test_fail_alloc(0);
    ray_distinct_guid_test_table_cap(0);
    ray_cdpg_test_fail_slots(0);
    ray_pool_test_ring_fail(0);
    ray_pool_test_cancel_at(0, false);
    ray_heap_test_scribble(false);
    ray_heap_test_pool_refuse(0);
    ray_file_writeback_test_hold(false);
    ray_file_writeback_test_fail(false);
    ray_sym_domain_test_hook(NULL);
    ray_alloc_fail_set_for_test(0, 0);
    agg_test_force_smallhash(false);
    agg_test_no_wide_dense(false);
    ray_clear_interrupt();
}

/* ---- Per-process scratch root (declared in test.h) ---- */

static char   g_tmp_root[4096];   /* TMPDIR/rayforce_t.<pid>, default /tmp, set in main() */
static size_t g_tmp_root_len;

/* A "/tmp" path starts at s[i]: "/tmp" as a whole path component, not part
 * of a longer name ("/var/tmp", "/tmpx"), and not already under the root. */
static bool tmp_hit(const char* s, size_t n, size_t i) {
    if (i + 4 > n || memcmp(s + i, "/tmp", 4) != 0) return false;
    if (i > 0) {
        char c = s[i - 1];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '.' || c == '-' || c == '/' || c == '~') return false;
    }
    char r = s[i + 4];
    if (!(r == '/' || r == '\0' || r == '"' || r == '\\' || r == '\'' || r == ' ' ||
          r == ')' || r == ';' || r == '\n' || r == '\t')) return false;
    bool under_root = strncmp(s + i, g_tmp_root, g_tmp_root_len) == 0 &&
                      (s[i + g_tmp_root_len] == '/' || s[i + g_tmp_root_len] == '\0');
    return !under_root;
}

/* The rewrite of `s` (malloc'd), or NULL when `s` names no "/tmp" path or
 * the root is not set yet. */
static char* tmp_subst(const char* s) {
    if (!g_tmp_root_len || !s) return NULL;
    size_t n = strlen(s), hits = 0;
    for (size_t i = 0; i < n; i++) if (tmp_hit(s, n, i)) hits++;
    if (!hits) return NULL;
    char* out = (char*)malloc(n + hits * g_tmp_root_len + 1);
    if (!out) { fprintf(stderr, "ray_test_tmp: out of memory\n"); abort(); }
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        if (tmp_hit(s, n, i)) { memcpy(out + o, g_tmp_root, g_tmp_root_len); o += g_tmp_root_len; i += 4; }
        else out[o++] = s[i++];
    }
    out[o] = '\0';
    return out;
}

/* Interned results: an open-addressed table keyed by the argument's text. */
typedef struct { char* key; char* val; } tmp_ent_t;
static tmp_ent_t* g_tmp_tab;
static size_t     g_tmp_cap, g_tmp_n;

static size_t tmp_hash(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return (size_t)h;
}
static char* tmp_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (!d) { fprintf(stderr, "ray_test_tmp: out of memory\n"); abort(); }
    return memcpy(d, s, n);
}

const char* ray_test_tmp_root(void) { return g_tmp_root; }

const char* ray_test_tmp(const char* s) {
    if (!s) return s;
    if (g_tmp_n * 2 >= g_tmp_cap) {
        size_t ncap = g_tmp_cap ? g_tmp_cap * 2 : 1024;
        tmp_ent_t* nt = (tmp_ent_t*)calloc(ncap, sizeof *nt);
        if (!nt) { fprintf(stderr, "ray_test_tmp: out of memory\n"); abort(); }
        for (size_t i = 0; i < g_tmp_cap; i++) {
            if (!g_tmp_tab[i].key) continue;
            size_t j = tmp_hash(g_tmp_tab[i].key) & (ncap - 1);
            while (nt[j].key) j = (j + 1) & (ncap - 1);
            nt[j] = g_tmp_tab[i];
        }
        free(g_tmp_tab); g_tmp_tab = nt; g_tmp_cap = ncap;
    }
    size_t j = tmp_hash(s) & (g_tmp_cap - 1);
    while (g_tmp_tab[j].key) {
        if (strcmp(g_tmp_tab[j].key, s) == 0) return g_tmp_tab[j].val;
        j = (j + 1) & (g_tmp_cap - 1);
    }
    char* v = tmp_subst(s);
    g_tmp_tab[j].key = tmp_strdup(s);
    g_tmp_tab[j].val = v ? v : g_tmp_tab[j].key;
    g_tmp_n++;
    return g_tmp_tab[j].val;
}

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
long ray_test_sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    switch (name) {
    case _SC_PAGESIZE:         return (long)si.dwPageSize;
    case _SC_NPROCESSORS_ONLN: return (long)si.dwNumberOfProcessors;
    case _SC_PHYS_PAGES: {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (!GlobalMemoryStatusEx(&ms)) return -1;
        return (long)(ms.ullTotalPhys / si.dwPageSize);
    }
    default: errno = EINVAL; return -1;
    }
}
#endif

/* ─── Equivalence pass ────────────────────────────────────────────────
 * Every save in these tests is written as generation 2 (absent compress
 * behaves as true) and every load reads it back: results must equal the
 * generation-0 run.  An assert on a generation-0 property (mapped-in-place
 * residency, an auto-built persisted index, header bytes) is written
 * generation-aware (TEST_ASSERT_LOADED_MMOD, a header check, or an explicit
 * {compress: false} save) so the rest of the test still runs here; a test
 * that cannot run twice in one process is listed in equiv_skip with the
 * reason.  Anything else that differs is a bug in the generation-2 path,
 * never a test change. */
static const char* const equiv_skip[] = {
    /* runs its own heap lifecycle and leaks a domain into the process-wide
     * cache on purpose: a second run in the same process (this pass) opens
     * no domain.  It saves a symbol file only, no table. */
    "equiv/domain/survives_heap_recycle",
    NULL,
};

static bool equiv_skipped(const char* name) {
    for (int i = 0; equiv_skip[i]; i++) if (strcmp(equiv_skip[i], name) == 0) return true;
    return false;
}

static bool rfl_file_saves(const char* path) {
    static const char* const forms[] = { ".db.splayed.set", ".csv.splayed", ".csv.parted",
                                         ".parquet.splayed", ".parquet.parted", NULL };
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    for (int i = 0; forms[i]; i++) if (strstr(buf, forms[i])) return true;
    return false;
}

static const test_entry_t* g_equiv_inner;
static bool g_equiv_on;
bool ray_test_equiv_pass(void) { return g_equiv_on; }
static void equiv_c_setup(void) {
    if (g_equiv_inner->setup) g_equiv_inner->setup();
    ray_store_test_force_compress(true);
    g_equiv_on = true;
}
static void equiv_c_teardown(void) {
    g_equiv_on = false;
    ray_store_test_force_compress(false);
    if (g_equiv_inner->teardown) g_equiv_inner->teardown();
}
static void equiv_rfl_setup(void)    { rfl_setup(); ray_store_test_force_compress(true); g_equiv_on = true; }
static void equiv_rfl_teardown(void) { g_equiv_on = false; ray_store_test_force_compress(false); rfl_teardown(); }

/* Wall-clock milliseconds (the summary line; scheduling in -j mode). */
static double now_wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* --quick leaves these out (prefix match), on top of the equivalence pass.
 * Each one costs seconds and covers a path the quick tier is not for:
 * randomized soak, multi-second oracle sweeps, large spill.  `make test`
 * (and CI) still runs every one of them. */
static const char* const quick_skip[] = {
    "stress/",          /* randomized soak against a shadow model */
    "stress_eval/",
    "large/",
    /* Ctrl-C has to land while ./rayforce is still computing: an unsanitized
     * build finishes first on a fast core.  Runs in make test-full (ASan). */
    "repl/pty/ctrl_c_during_lazy_materialize",
    NULL,
};
#if !defined(_WIN32)   /* -j only: the parallel runner is POSIX-only */
/* -j runs these after every other test, one at a time, on an otherwise idle
 * machine (prefix match): tests whose outcome depends on which pool worker
 * thread picks up which task (with every core busy, the scheduler reorders
 * them), and pairs that must not overlap.  Each costs well under a second;
 * coverage is the same. */
static const char* const exclusive_tests[] = {
    "heap/relieve_",               /* which worker heap keeps which empty pool */
    "heap/raw_discard",            /* process-wide anon / direct byte counts */
    "rfl/query/grouped_grade_per_group",        /* in-run route-cost comparisons ... */
    "equiv/rfl/query/grouped_grade_per_group",  /* ... that a saturated machine skews */
    NULL,
};
/* -j hands these out first, so each is the first test of a newly forked
 * worker.  For tests whose arithmetic assumes a small process footprint:
 * they size a memory watermark from it, and a worker that already ran
 * hundreds of tests holds several hundred MB more than a fresh one. */
static const char* const fresh_tests[] = {
    "agg_contract/dense_wide_budget",   /* also _sel: per-worker slab budget */
    "rfl/io/csv_escaped_cells",         /* bounds the read's rise of the anon peak */
    NULL,
};
static bool fresh_test(const char* name) {
    for (int i = 0; fresh_tests[i]; i++)
        if (strncmp(name, fresh_tests[i], strlen(fresh_tests[i])) == 0) return true;
    return false;
}

static bool exclusive_test(const char* name) {
    for (int i = 0; exclusive_tests[i]; i++)
        if (strncmp(name, exclusive_tests[i], strlen(exclusive_tests[i])) == 0) return true;
    return false;
}
#endif

static bool quick_skipped(const char* name) {
    for (int i = 0; quick_skip[i]; i++)
        if (strncmp(name, quick_skip[i], strlen(quick_skip[i])) == 0) return true;
    return false;
}

/* ─── Job list ────────────────────────────────────────────────────── */

/* Every test the run will execute, in serial order: the compiled-in groups,
 * the .rfl files, then the equivalence pass.  `inner` is the wrapped entry
 * of an equiv/ C test (g_equiv_inner while it runs). */
typedef struct {
    test_entry_t        e;
    const test_entry_t* inner;
    double              est_ms;   /* last recorded wall time; -1 = unknown */
    int                 rfl;      /* the .rfl slot an rfl_thunk job runs; -1 = none */
} test_job_t;

static test_job_t* g_jobs;
static int         g_njobs, g_jobs_cap;

static void job_add(test_entry_t e, const test_entry_t* inner) {
    if (g_njobs == g_jobs_cap) {
        g_jobs_cap = g_jobs_cap ? g_jobs_cap * 2 : 8192;
        g_jobs = (test_job_t*)realloc(g_jobs, (size_t)g_jobs_cap * sizeof *g_jobs);
        if (!g_jobs) { fprintf(stderr, "test driver: out of memory\n"); exit(2); }
    }
    g_jobs[g_njobs++] = (test_job_t){ .e = e, .inner = inner, .est_ms = -1, .rfl = -1 };
}
static void job_add_rfl(test_entry_t e, int slot) {
    job_add(e, NULL);
    g_jobs[g_njobs - 1].rfl = slot;
}

static void collect_jobs(const char* filter, bool quick) {
    /* Compiled-in groups */
    for (int gi = 0; compiled_groups[gi]; gi++) {
        const test_entry_t* g = compiled_groups[gi];
        for (int i = 0; g[i].name; i++) {
            if (!name_matches_filter(g[i].name, filter)) continue;
            /* large/ tests need several GiB of RAM and disk: only on request */
            if (strncmp(g[i].name, "large/", 6) == 0 &&
                !(filter && strncmp(filter, "large/", 6) == 0)) continue;
            if (quick && quick_skipped(g[i].name)) continue;
            job_add(g[i], NULL);
        }
    }

    /* Dynamic .rfl group */
    for (int i = 0; i < g_rfl_count; i++) {
        if (!name_matches_filter(g_rfl[i].name, filter)) continue;
        if (quick && quick_skipped(g_rfl[i].name)) continue;
        job_add_rfl((test_entry_t){ .name = g_rfl[i].name, .func = rfl_thunk,
                                    .setup = rfl_setup, .teardown = rfl_teardown }, i);
    }

    /* Equivalence pass: C groups that save and reload tables, then every
     * .rfl file that saves one.  --quick leaves the whole pass out. */
    if (quick) return;
    static const test_entry_t* const equiv_groups[] = {
        splay_entries, store_entries, partition_exec_entries, link_entries,
        /* query-heavy groups that save tables */
        stress_eval_entries, stress_matrix_entries, stress_random_entries,
        lang_entries, index_entries, csr_entries, domain_entries, sym_entries,
        NULL,
    };
    char ename[600];
    for (int gi = 0; equiv_groups[gi]; gi++) {
        for (int i = 0; equiv_groups[gi][i].name; i++) {
            snprintf(ename, sizeof ename, "equiv/%s", equiv_groups[gi][i].name);
            if (!name_matches_filter(ename, filter) || equiv_skipped(ename)) continue;
            job_add((test_entry_t){ .name = tmp_strdup(ename), .func = equiv_groups[gi][i].func,
                                    .setup = equiv_c_setup, .teardown = equiv_c_teardown },
                    &equiv_groups[gi][i]);
        }
    }
    for (int i = 0; i < g_rfl_count; i++) {
        snprintf(ename, sizeof ename, "equiv/%.255s", g_rfl[i].name);
        if (!name_matches_filter(ename, filter) || equiv_skipped(ename)) continue;
        if (!rfl_file_saves(g_rfl[i].path)) continue;
        job_add_rfl((test_entry_t){ .name = tmp_strdup(ename), .func = rfl_thunk,
                                    .setup = equiv_rfl_setup, .teardown = equiv_rfl_teardown }, i);
    }
}

/* Keep only the jobs named in `path`, in the file's order. */
static int replay_select(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    /* the selection, then every job behind it (past g_njobs): the full
     * list stays reachable, it owns the equiv/ names (tmp_strdup) */
    test_job_t* out = (test_job_t*)malloc((size_t)(g_njobs > 0 ? 2 * g_njobs : 1) * sizeof *out);
    if (!out) { fclose(f); return -1; }
    int n = 0;
    char line[700];
    while (fgets(line, sizeof line, f) && n < g_njobs) {
        line[strcspn(line, "\r\n")] = '\0';
        if (!*line) continue;
        int i = 0;
        while (i < g_njobs && strcmp(g_jobs[i].e.name, line) != 0) i++;
        if (i == g_njobs) { fprintf(stderr, "replay: no test named %s\n", line); continue; }
        out[n++] = g_jobs[i];
    }
    fclose(f);
    memcpy(out + n, g_jobs, (size_t)g_njobs * sizeof *out);
    int total = g_njobs;
    free(g_jobs);
    g_jobs = out; g_njobs = n; g_jobs_cap = n + total;
    return 0;
}

/* Runs one job in this process; returns its status. */
static test_status_t run_job(const test_job_t* j, int* pass, int* fail, int* skip) {
    int p0 = *pass, s0 = *skip;
    g_equiv_inner = j->inner;
    g_rfl_cur = j->rfl;
    run_one(&j->e, pass, fail, skip);
    g_rfl_cur = -1;
    g_equiv_inner = NULL;
    return *pass != p0 ? TEST_PASS : *skip != s0 ? TEST_SKIP : TEST_FAIL;
}

/* Point this process's scratch root at /tmp/rayforce_t.<pid> and create it.
 * A forked worker calls it again so no two processes share a file. */
static int tmp_root_init(void) {
    const char* test_tmp = getenv("TMPDIR");
    if (!test_tmp || !*test_tmp) test_tmp = "/tmp";
    int tmp_len = snprintf(g_tmp_root, sizeof g_tmp_root, "%s/rayforce_t.%ld", test_tmp, (long)getpid());
    if (tmp_len < 0 || (size_t)tmp_len >= sizeof g_tmp_root) {
        fprintf(stderr, "TMPDIR path too long\n"); return -1;
    }
    (void)ray_test_rm_rf(g_tmp_root);                    /* a dead run's leftovers under a reused pid */
    if (ray_test_mkdir_p(g_tmp_root) != 0) { fprintf(stderr, "cannot create %s\n", g_tmp_root); return -1; }
    g_tmp_root_len = strlen(g_tmp_root);
    /* forget rewrites interned under a previous root */
    g_tmp_tab = NULL; g_tmp_cap = 0; g_tmp_n = 0;
    return 0;
}

/* Run from a private working directory: <root>/cwd, holding a symlink to
 * each top-level entry of the directory the suite started in (test/, src/,
 * ./rayforce ...).  Tests that read repo files by relative path still find
 * them; files a test creates by relative path ("rf_test_x.csv") land in the
 * private directory, so -j workers never share one and the checkout stays
 * clean.  Names a run leaves behind (rf_*, rfl_*, dotfiles ...) are not
 * linked, so a stale copy in the checkout is never written through. */
#if !defined(_WIN32)
static char g_orig_cwd[4096];

static bool cwd_link_skipped(const char* n) {
    static const char* const pfx[] = { ".", "rf_", "rfl_", "ray_", "__", "rayforce.test.times", NULL };
    for (int i = 0; pfx[i]; i++) if (strncmp(n, pfx[i], strlen(pfx[i])) == 0) return true;
    return false;
}

static int private_cwd_enter(void) {
    if (!g_orig_cwd[0] && !getcwd(g_orig_cwd, sizeof g_orig_cwd)) return -1;
    char dir[sizeof g_tmp_root + 8], link[sizeof dir + 512], target[sizeof g_orig_cwd + 512];
    snprintf(dir, sizeof dir, "%s/cwd", g_tmp_root);
    if (ray_test_mkdir_p(dir) != 0) return -1;
    DIR* d = opendir(g_orig_cwd);
    if (!d) return -1;
    for (struct dirent* e; (e = readdir(d)); ) {
        if (cwd_link_skipped(e->d_name)) continue;
        snprintf(link, sizeof link, "%s/%s", dir, e->d_name);
        snprintf(target, sizeof target, "%s/%s", g_orig_cwd, e->d_name);
        if (symlink(target, link) != 0) { /* an entry the test never reads */ }
    }
    closedir(d);
    return chdir(dir);
}

/* Back to the starting directory, so the root can be removed. */
static void private_cwd_leave(void) {
    if (g_orig_cwd[0] && chdir(g_orig_cwd) != 0) perror(g_orig_cwd);
}

const char* ray_test_launch_dir(void) { return g_orig_cwd[0] ? g_orig_cwd : NULL; }
#else
static int  private_cwd_enter(void) { return 0; }
static void private_cwd_leave(void) {}
const char* ray_test_launch_dir(void) { return NULL; }
#endif

/* ─── Recorded timings (parallel scheduling) ─────────────────────── */

/* "<binary>.times" holds "<ms> <name>" per test from the last parallel run.
 * Jobs are dispatched longest first, so one slow test does not start last
 * and leave every other worker idle while it finishes. */
static int job_cmp_name(const void* a, const void* b) {
    return strcmp(((const test_job_t*)a)->e.name, ((const test_job_t*)b)->e.name);
}
static int job_cmp_est_desc(const void* a, const void* b) {
    double x = ((const test_job_t*)a)->est_ms, y = ((const test_job_t*)b)->est_ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void times_load(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    qsort(g_jobs, (size_t)g_njobs, sizeof *g_jobs, job_cmp_name);
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char* sp = strchr(line, ' ');
        if (!sp) continue;
        char* nl = strchr(sp + 1, '\n');
        if (nl) *nl = '\0';
        test_job_t key = { .e = { .name = sp + 1 } };
        test_job_t* hit = (test_job_t*)bsearch(&key, g_jobs, (size_t)g_njobs, sizeof *g_jobs, job_cmp_name);
        if (hit) hit->est_ms = strtod(line, NULL);
    }
    fclose(f);
}

static void times_save(const char* path, const double* ms) {
    /* keep entries for tests this (possibly filtered) run did not execute */
    size_t cap = 1 << 20, len = 0;
    char* old = (char*)malloc(cap);
    FILE* f = old ? fopen(path, "r") : NULL;
    if (f) { len = fread(old, 1, cap - 1, f); fclose(f); }
    if (old) old[len] = '\0';
    char tmp[616];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* o = fopen(tmp, "w");
    if (!o) { free(old); return; }
    for (int i = 0; i < g_njobs; i++)
        if (ms[i] >= 0) fprintf(o, "%.1f %s\n", ms[i], g_jobs[i].e.name);
    for (char* ln = old; ln && *ln; ) {
        char* nl = strchr(ln, '\n');
        if (nl) *nl = '\0';
        char* sp = strchr(ln, ' ');
        bool ran = false;
        if (sp) for (int i = 0; i < g_njobs && !ran; i++)
            ran = ms[i] >= 0 && strcmp(g_jobs[i].e.name, sp + 1) == 0;
        if (sp && !ran) fprintf(o, "%s\n", ln);
        ln = nl ? nl + 1 : NULL;
    }
    fclose(o);
    free(old);
    (void)rename(tmp, path);
}

#if !defined(_WIN32)
/* ─── Parallel runner (-j N) ─────────────────────────────────────── */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

/* A worker is a fork of the driver taken before any test ran.  It reads job
 * indices from its command pipe, runs each with run_job() (output into its
 * own log file), and answers on the shared result pipe.  Records are smaller
 * than PIPE_BUF, so writes from different workers never interleave. */
typedef struct { int32_t job; int32_t status; int32_t retire; int32_t pad; int64_t log_end; } worker_res_t;

typedef struct {
    pid_t   pid;
    int     cmd_fd;      /* parent -> worker: int32 job index, -1 = exit */
    int     log_fd;      /* parent's read handle on the worker's log      */
    int     job;         /* job in flight, -1 when idle                   */
    int64_t log_pos;     /* log offset where the job in flight started    */
    double  t0;          /* wall-clock dispatch time (ms)                 */
    bool    retire;      /* it left spill pools: exits, a fresh one follows */
    bool    timed_out;   /* killed for running one test past --timeout */
    int*    hist;        /* every job this process ran, in order (replay) */
    int     nhist, hist_cap;
} worker_t;

static double now_ms(void) { return now_wall_ms(); }

static void set_cloexec(int fd) { (void)fcntl(fd, F_SETFD, FD_CLOEXEC); }

static bool write_all(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w; n -= (size_t)w;
    }
    return true;
}

static bool read_all(int fd, void* buf, size_t n) {
    char* p = (char*)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r; n -= (size_t)r;
    }
    return true;
}

/* Copy the worker log [from, to) (to < 0: to EOF) onto stdout. */
static void log_copy(int fd, int64_t from, int64_t to) {
    char buf[65536];
    int64_t pos = from;
    for (;;) {
        size_t want = sizeof buf;
        if (to >= 0) {
            if (pos >= to) break;
            if ((int64_t)want > to - pos) want = (size_t)(to - pos);
        }
        ssize_t r = pread(fd, buf, want, (off_t)pos);
        if (r <= 0) break;
        fwrite(buf, 1, (size_t)r, stdout);
        pos += r;
    }
    fflush(stdout);
}

typedef struct {
    worker_t* w;
    int       nw;
    int       res_rd, res_wr;
    double*   ms;
    int       next;                 /* next job index to dispatch */
    int*      pass; int* fail; int* skip;
} pool_t;

/* Whether any heap of this process holds a pool spilled to a file.  Spill
 * mappings are MAP_SHARED (mem/heap.c): after fork() parent and child write
 * the same pages, so a later test that forks and keeps running rayforce code
 * in the child corrupts both heaps.  A test that tightened the memory
 * watermark can leave such pools behind; the worker then retires, and the
 * next tests run in a fresh process, as they would after it in a serial run
 * only by luck of the order. */
static bool process_has_spill_pools(void) {
    for (int i = 0; i < RAY_HEAP_REGISTRY_SIZE; i++) {
        const ray_heap_t* h = ray_heap_registry[i];
        if (!h) continue;
        for (uint32_t p = 0; p < h->pool_count && p < RAY_MAX_POOLS; p++)
            if (h->pools[p].backed) return true;
    }
    return false;
}

static void worker_main(pool_t* P, int cmd_fd, int log_fd) {
    /* drop the parent's handles on the other workers */
    for (int k = 0; k < P->nw; k++) {
        if (P->w[k].cmd_fd >= 0) close(P->w[k].cmd_fd);
        if (P->w[k].log_fd >= 0) close(P->w[k].log_fd);
    }
    close(P->res_rd);
    dup2(log_fd, STDOUT_FILENO);
    dup2(log_fd, STDERR_FILENO);
    close(log_fd);
    if (tmp_root_init() != 0 || private_cwd_enter() != 0) _exit(2);
    int pass = 0, fail = 0, skip = 0;
    int32_t j;
    while (read_all(cmd_fd, &j, sizeof j) && j >= 0 && j < g_njobs) {
        test_status_t st = run_job(&g_jobs[j], &pass, &fail, &skip);
        fflush(stdout); fflush(stderr);
        bool retire = process_has_spill_pools();
        worker_res_t r = { .job = j, .status = (int32_t)st, .retire = retire,
                           .log_end = (int64_t)lseek(STDOUT_FILENO, 0, SEEK_CUR) };
        if (!write_all(P->res_wr, &r, sizeof r) || retire) break;
    }
    private_cwd_leave();
    (void)ray_test_rm_rf(g_tmp_root);
    fflush(stdout); fflush(stderr);
    exit(0);   /* a normal exit, so LeakSanitizer still reports at the end */
}

/* Fork worker k with a fresh log.  False when it cannot be started. */
static bool worker_spawn(pool_t* P, int k) {
    worker_t* me = &P->w[k];
    char path[sizeof g_tmp_root + 64];
    snprintf(path, sizeof path, "%s/worker%d.%d.log", g_tmp_root, k, P->next);
    int log_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (log_fd < 0) { perror(path); return false; }
    (void)unlink(path);   /* both sides keep it open; nothing to clean up */
    int cmd[2];
    if (pipe(cmd) != 0) { close(log_fd); perror("pipe"); return false; }
    set_cloexec(cmd[0]); set_cloexec(cmd[1]);
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); close(log_fd); close(cmd[0]); close(cmd[1]); return false; }
    if (pid == 0) {
        close(cmd[1]);
        me->cmd_fd = -1; me->log_fd = -1;
        worker_main(P, cmd[0], log_fd);
    }
    close(cmd[0]);
    int rd = dup(log_fd);   /* pread handle independent of the worker's offset */
    close(log_fd);
    set_cloexec(rd);
    *me = (worker_t){ .pid = pid, .cmd_fd = cmd[1], .log_fd = rd, .job = -1, .log_pos = 0,
                      .hist = me->hist, .hist_cap = me->hist_cap, .nhist = 0 };
    return true;
}

/* Give worker k its next job, or tell it to exit when none is left. */
static void worker_feed(pool_t* P, int k) {
    worker_t* me = &P->w[k];
    int32_t j = P->next < g_njobs ? P->next++ : -1;
    me->job = j; me->t0 = now_ms();
    if (j >= 0) {
        if (me->nhist == me->hist_cap) {
            me->hist_cap = me->hist_cap ? me->hist_cap * 2 : 256;
            me->hist = (int*)realloc(me->hist, (size_t)me->hist_cap * sizeof *me->hist);
            if (!me->hist) { fprintf(stderr, "test driver: out of memory\n"); exit(2); }
        }
        me->hist[me->nhist++] = j;
    }
    if (!write_all(me->cmd_fd, &j, sizeof j) && j >= 0) { me->job = -1; P->next--; return; }
    if (j < 0) { close(me->cmd_fd); me->cmd_fd = -1; }
}

static void replay_note(const worker_t* me, int job);

/* Read every queued result; for each, print the test's output, count it and
 * feed that worker again (unless it already exited). */
static void results_drain(pool_t* P, int timeout_ms) {
    for (;;) {
        struct pollfd pfd = { .fd = P->res_rd, .events = POLLIN };
        if (poll(&pfd, 1, timeout_ms) <= 0 || !(pfd.revents & POLLIN)) return;
        timeout_ms = 0;
        worker_res_t r;
        if (!read_all(P->res_rd, &r, sizeof r)) return;
        int k = 0;
        while (k < P->nw && !(P->w[k].job == r.job && P->w[k].log_fd >= 0)) k++;
        if (k == P->nw) continue;
        worker_t* me = &P->w[k];
        P->ms[r.job] = now_ms() - me->t0;
        log_copy(me->log_fd, me->log_pos, r.log_end);
        me->log_pos = r.log_end;
        if      (r.status == TEST_PASS) (*P->pass)++;
        else if (r.status == TEST_SKIP) (*P->skip)++;
        else                          { (*P->fail)++; replay_note(me, r.job); }
        fflush(stdout);
        me->job = -1;
        if (r.retire) {
            me->retire = true;
            if (me->cmd_fd >= 0) { close(me->cmd_fd); me->cmd_fd = -1; }
        } else if (me->pid > 0) {
            worker_feed(P, k);
        }
    }
}

/* A test that fails in a worker may depend on what that process ran before
 * it.  Write the worker's history up to and including it, one name a line,
 * for `--replay FILE` to run in that order in one process. */
static const char* g_argv0 = "./rayforce.test";

/* This binary's path, for a test child that execs it (test_repl.c). */
const char* test_self_exe(void) {
#if defined(__linux__)
    if (access("/proc/self/exe", X_OK) == 0) return "/proc/self/exe";
#endif
    return g_argv0;
}
int repl_child_main(const char* variant);   /* test_repl.c */

/* The children test_child_spawn can run (test.h), each defined beside its test. */
#if !defined(_WIN32)
int store_child_col_close_failure(int argc, char** argv);
int store_child_col_append_index_rollback(int argc, char** argv);
int store_child_stream_index_write_failure(int argc, char** argv);
int store_child_hash_index_marker_last(int argc, char** argv);
int store_child_hash_index_rollback(int argc, char** argv);
int cs_child_stream_gen2_transcode_failure(int argc, char** argv);
int dom_child_append_failed(int argc, char** argv);
int jrn_child_lost_roll(int argc, char** argv);
int jrn_child_open_stage(int argc, char** argv);
int pq_child_sk_huge_raw(int argc, char** argv);
int pool_child_spawn_fails(int argc, char** argv);
static const struct { const char* name; test_child_fn fn; } g_children[] = {
    { "store/col_close_failure",         store_child_col_close_failure },
    { "store/col_append_index_rollback", store_child_col_append_index_rollback },
    { "store/stream_index_write_failure", store_child_stream_index_write_failure },
    { "store/hash_index_marker_last",    store_child_hash_index_marker_last },
    { "store/hash_index_rollback",       store_child_hash_index_rollback },
    { "compress/stream_gen2_transcode_failure", cs_child_stream_gen2_transcode_failure },
    { "domain/append_failed",            dom_child_append_failed },
    { "journal/lost_roll",               jrn_child_lost_roll },
    { "journal/open_stage",              jrn_child_open_stage },
    { "parquet/sk_huge_raw",             pq_child_sk_huge_raw },
    { "pool/spawn_fails",                pool_child_spawn_fails },
};

int test_child_spawn(const char* name, const char* const* args) {
    const char* argv[16];
    int n = 0;
    argv[n++] = test_self_exe();
    argv[n++] = "--child";
    argv[n++] = name;
    for (int i = 0; args && args[i] && n < 15; i++) argv[n++] = args[i];
    argv[n] = NULL;
    fflush(stdout); fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }
    return pid;
}

int test_child_run(const char* name, const char* const* args) {
    int pid = test_child_spawn(name, args);
    if (pid < 0) return -1;
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return -1;
}

static int child_main(int argc, char** argv) {
    for (size_t i = 0; i < sizeof g_children / sizeof g_children[0]; i++)
        if (strcmp(argv[0], g_children[i].name) == 0)
            _exit(g_children[i].fn(argc - 1, argv + 1));
    fprintf(stderr, "--child: no child %s\n", argv[0]);
    _exit(126);
}
#endif
static void replay_note(const worker_t* me, int job) {
    static int seq;
    char path[128];
    snprintf(path, sizeof path, "/tmp/rayforce_replay.%ld.%d", (long)getpid(), ++seq);
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < me->nhist; i++) {
        fprintf(f, "%s\n", g_jobs[me->hist[i]].e.name);
        if (me->hist[i] == job) break;
    }
    fclose(f);
    int before = me->nhist - 1;
    printf("        to reproduce (after the %d test%s that ran before it in this worker, in one process):\n"
           "          %s --replay %s\n",
           before, before == 1 ? "" : "s", g_argv0, path);
}

static double g_test_timeout_ms = 600e3;   /* --timeout: per test, -j only */

static int run_parallel(int nworkers, const char* times_path,
                        int* pass, int* fail, int* skip) {
    signal(SIGPIPE, SIG_IGN);
    if (times_path) times_load(times_path);
    /* longest first; unknown ones after, in their serial order */
    for (int i = 0; i < g_njobs; i++)
        if (g_jobs[i].est_ms < 0) g_jobs[i].est_ms = -1.0 - (double)i;
    qsort(g_jobs, (size_t)g_njobs, sizeof *g_jobs, job_cmp_est_desc);
    /* fresh-process tests first, keeping the order of the rest: the first
     * job each worker gets runs in a process that has run nothing */
    {
        test_job_t* tmp = (test_job_t*)malloc((size_t)(g_njobs > 0 ? g_njobs : 1) * sizeof *tmp);
        if (tmp) {
            int o = 0;
            for (int i = 0; i < g_njobs; i++) if (fresh_test(g_jobs[i].e.name))  tmp[o++] = g_jobs[i];
            for (int i = 0; i < g_njobs; i++) if (!fresh_test(g_jobs[i].e.name)) tmp[o++] = g_jobs[i];
            memcpy(g_jobs, tmp, (size_t)g_njobs * sizeof *tmp);
            free(tmp);
        }
    }

    int rp[2];
    if (pipe(rp) != 0) { perror("pipe"); return -1; }
    set_cloexec(rp[0]); set_cloexec(rp[1]);

    pool_t P = { .res_rd = rp[0], .res_wr = rp[1], .pass = pass, .fail = fail, .skip = skip };
    P.nw = nworkers < g_njobs ? nworkers : g_njobs;
    if (P.nw < 1) P.nw = 1;
    P.w  = (worker_t*)calloc((size_t)P.nw, sizeof *P.w);
    P.ms = (double*)malloc((size_t)(g_njobs > 0 ? g_njobs : 1) * sizeof *P.ms);
    if (!P.w || !P.ms) { fprintf(stderr, "test driver: out of memory\n"); return -1; }
    for (int k = 0; k < P.nw; k++) P.w[k] = (worker_t){ .cmd_fd = -1, .log_fd = -1, .job = -1 };
    for (int i = 0; i < g_njobs; i++) P.ms[i] = -1;

    int alive = 0, extra_fail = 0;
    for (int k = 0; k < P.nw; k++) if (worker_spawn(&P, k)) alive++;
    if (!alive) { fprintf(stderr, "test driver: could not start workers\n"); return -1; }
    for (int k = 0; k < P.nw; k++) if (P.w[k].pid > 0) worker_feed(&P, k);

    while (alive > 0) {
        results_drain(&P, 200);
        /* a test that hangs takes its worker down, not the run */
        for (int k = 0; k < P.nw; k++)
            if (P.w[k].pid > 0 && P.w[k].job >= 0 && !P.w[k].timed_out &&
                now_ms() - P.w[k].t0 > g_test_timeout_ms) {
                P.w[k].timed_out = true;
                kill(P.w[k].pid, SIGKILL);
            }
        for (int k = 0; k < P.nw; k++) {
            worker_t* me = &P.w[k];
            if (me->pid <= 0) continue;
            int st;
            if (waitpid(me->pid, &st, WNOHANG) != me->pid) continue;
            pid_t dead = me->pid;
            me->pid = 0;
            alive--;
            /* results it wrote just before exiting are still in the pipe */
            results_drain(&P, 0);
            char why[96];
            if (me->timed_out)   snprintf(why, sizeof why, "killed: the test ran past --timeout %g s", g_test_timeout_ms / 1e3);
            else if (WIFSIGNALED(st)) snprintf(why, sizeof why, "killed by signal %d (%s)", WTERMSIG(st), strsignal(WTERMSIG(st)));
            else                 snprintf(why, sizeof why, "exited with status %d", WEXITSTATUS(st));
            bool crashed = me->job >= 0;
            if (crashed) {
                log_copy(me->log_fd, me->log_pos, -1);
                printf("\n        %sFAIL%s  worker %s while running %s\n",
                       C_RED, C_RESET, why, g_jobs[me->job].e.name);
                replay_note(me, me->job);
                P.ms[me->job] = now_ms() - me->t0;
                (*fail)++;
            } else if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0)) {
                /* after its last test: LeakSanitizer or an atexit check */
                log_copy(me->log_fd, me->log_pos, -1);
                printf("%sFAIL%s  worker %ld %s after its last test (output above)\n",
                       C_RED, C_RESET, (long)dead, why);
                extra_fail++;
            }
            fflush(stdout);
            close(me->log_fd); me->log_fd = -1;
            if (me->cmd_fd >= 0) { close(me->cmd_fd); me->cmd_fd = -1; }
            me->job = -1;
            /* replace a crashed or retired worker while work remains */
            if ((crashed || me->retire) && P.next < g_njobs && worker_spawn(&P, k)) {
                alive++;
                worker_feed(&P, k);
            }
        }
    }
    close(P.res_rd); close(P.res_wr);
    if (times_path) times_save(times_path, P.ms);
    free(P.ms);
    for (int k = 0; k < P.nw; k++) free(P.w[k].hist);
    free(P.w);
    *fail += extra_fail;
    return 0;
}
#endif

int main(int argc, char** argv) {
#if !defined(_WIN32)
    g_argv0 = argv[0];
    /* A REPL test's child: a fresh process running one REPL (test_repl.c). */
    if (argc == 3 && strcmp(argv[1], "--repl-child") == 0)
        return repl_child_main(argv[2]);
    /* A test's child (test_child_spawn): a fresh process. */
    if (argc >= 3 && strcmp(argv[1], "--child") == 0)
        return child_main(argc - 2, argv + 2);
#endif
#if defined(_WIN32)
    (void)_mkdir("/tmp");   /* tests use "/tmp/..." paths (see test.h) */
#endif
    ray_expr_stats_init();
    ray_idx_stats_init();
    g_color = isatty(fileno(stdout));

    const char* filter = NULL;
    int  jobs  = 1;
    bool quick = false;
    const char* replay = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--filter") == 0 || strcmp(argv[i], "-f") == 0)
            && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--census") == 0 && i + 1 < argc) {
            g_census = fopen(argv[++i], "w");
            if (!g_census) { fprintf(stderr, "cannot open census file\n"); return 2; }
        } else if ((strcmp(argv[i], "--jobs") == 0 || strcmp(argv[i], "-j") == 0) && i + 1 < argc) {
            jobs = atoi(argv[++i]);
#if defined(_SC_NPROCESSORS_ONLN)   /* hidden by _POSIX_C_SOURCE on macOS */
            if (jobs <= 0) jobs = (int)sysconf(_SC_NPROCESSORS_ONLN);
#endif
            if (jobs <= 0) jobs = 4;
        } else if (strcmp(argv[i], "--quick") == 0) {
            quick = true;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
#if !defined(_WIN32)
            g_test_timeout_ms = atof(argv[i + 1]) * 1e3;
            if (g_test_timeout_ms <= 0) g_test_timeout_ms = 1e12;
#endif
            i++;
        } else if (strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            replay = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [-f SUBSTR] [-j N] [--quick] [--census PATH]\n", argv[0]);
            printf("  -f, --filter SUBSTR   Only run tests whose name contains SUBSTR.\n");
            printf("  -j, --jobs N          Run tests in N worker processes (0 = one per CPU).\n");
            printf("  --timeout SEC         With -j: fail a test that runs longer (default 600; 0 = none).\n");
            printf("  --quick               Leave out the equivalence pass and the slow tests\n");
            printf("                        listed in quick_skip (test/main.c).\n");
            printf("  --replay FILE         Run the tests named in FILE (one a line), in that order,\n");
            printf("                        in one process: what -j prints for a failure.\n");
            printf("  --census PATH         Record .rfl lines whose grouping ran on the legacy ladder.\n");
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    /* the census file is one stream written test by test: keep it serial */
    if (g_census) jobs = 1;
#if defined(_WIN32)
    jobs = 1;   /* no fork(): the parallel runner is POSIX only */
#endif

    if (g_census) setvbuf(g_census, NULL, _IOLBF, 0);   /* closed by the OS at exit; lines land as written */
    if (tmp_root_init() != 0) return 2;
    const char* rfl_root = getenv("RFL_ROOT");
    if (!rfl_root || !*rfl_root) rfl_root = "test/rfl";
    if (rfl_scan(rfl_root) < 0) return 2;

    collect_jobs(replay ? NULL : filter, replay ? false : quick);
    if (replay && replay_select(replay) != 0) return 2;
    if (replay) jobs = 1;

    int pass = 0, fail = 0, skip = 0, total = g_njobs;
    double t0 = now_wall_ms();
    if (jobs > 1 && g_njobs > 1) {
#if !defined(_WIN32)
        char times_path[600];
        snprintf(times_path, sizeof times_path, "%s.times", argv[0]);
        printf("running %d tests in %d workers\n", g_njobs, jobs < g_njobs ? jobs : g_njobs);
        fflush(stdout);
        /* exclusive tests last, in one worker of their own */
        test_job_t* all = g_jobs;
        int nall = g_njobs, nx = 0;
        for (int i = 0; i < nall; i++)
            if (!exclusive_test(all[i].e.name)) { test_job_t t = all[i]; all[i] = all[nx]; all[nx++] = t; }
        int phases[2][2] = { { 0, nx }, { nx, nall - nx } };
        for (int ph = 0; ph < 2; ph++) {
            if (phases[ph][1] == 0) continue;
            g_jobs = all + phases[ph][0]; g_njobs = phases[ph][1];
            if (ph == 1) { printf("running %d exclusive tests one at a time\n", g_njobs); fflush(stdout); }
            if (run_parallel(ph == 0 ? jobs : 1, times_path, &pass, &fail, &skip) != 0) return 2;
        }
        g_jobs = all; g_njobs = nall;
#endif
    } else {
        if (private_cwd_enter() != 0) { fprintf(stderr, "cannot enter a private working directory\n"); return 2; }
        for (int i = 0; i < g_njobs; i++) run_job(&g_jobs[i], &pass, &fail, &skip);
    }

    private_cwd_leave();
    (void)ray_test_rm_rf(g_tmp_root);

    printf("\n%s=== %d of %d passed (%d skipped, %d failed) in %.1f s ===%s\n",
           fail ? C_RED : (skip ? C_YELLOW : C_GREEN),
           pass, total, skip, fail, (now_wall_ms() - t0) / 1e3, C_RESET);

    return fail ? 1 : 0;
}
