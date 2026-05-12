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
 * Coverage tests for src/ops/dump.c — the *query plan* dumper.
 *
 * Reachable surface:
 *   1. ray_opcode_name(opcode) — switch over all OP_* codes.
 *   2. type_name(t)            — switch over RAY_* output type tags.
 *   3. dump_node()             — annotation + recursion logic.
 *   4. ray_graph_dump()        — header/footer + dispatch to dump_node.
 *
 * The exec/graph_dump test in test_exec.c only exercises a tiny FILTER+SUM
 * pipeline; most of dump.c's switch arms remain cold.  These tests construct
 * tailored graphs that hit the remaining branches by directly calling
 * ray_graph_dump on a buffer-backed FILE* and asserting on the formatted
 * substrings.
 */

#define _POSIX_C_SOURCE 200809L

#include "test.h"
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"
#include "table/sym.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ---------- Helpers ---------- */

/* Dump a graph to a caller-supplied stack buffer via tmpfile() + fread.
 * Avoids any libc heap allocation (per project rules: no malloc/free in
 * tests).  tmpfile() opens an unlinked file under /tmp; fclose deletes
 * it — no on-disk leak.
 *
 * `cap` must be the buffer capacity (including space for trailing NUL).
 * The buffer is always NUL-terminated even if the dump exceeds cap-1.
 *
 * Returns the formatted text length actually copied (0..cap-1).
 */
static size_t dump_to_buf(ray_graph_t* g, ray_op_t* root, char* buf, size_t cap) {
    if (cap == 0) return 0;
    buf[0] = '\0';
    FILE* f = tmpfile();
    if (!f) return 0;
    ray_graph_dump(g, root, f);
    fflush(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n;
}

/* Helper: small 1-row table for SCAN nodes. */
static ray_t* make_tiny_table(void) {
    (void)ray_sym_init();
    int64_t v[]  = { 42 };
    double  fv[] = { 3.14 };
    ray_t* a = ray_vec_from_raw(RAY_I64, v,  1);
    ray_t* b = ray_vec_from_raw(RAY_F64, fv, 1);
    int64_t n_a = ray_sym_intern("a", 1);
    int64_t n_b = ray_sym_intern("b", 1);
    ray_t* tbl  = ray_table_new(2);
    tbl = ray_table_add_col(tbl, n_a, a);
    tbl = ray_table_add_col(tbl, n_b, b);
    ray_release(a);
    ray_release(b);
    return tbl;
}

/* ---------- Tests for ray_opcode_name() ---------- */

/* ray_opcode_name is a pure switch — verify every documented opcode produces
 * the expected mnemonic, plus "UNKNOWN" for an unrecognised value.  Doing it
 * via the public symbol covers all switch arms regardless of whether the
 * opcode appears in any dump_node graph. */
static test_result_t test_dump_opcode_name_all(void) {
    struct { uint16_t op; const char* name; } cases[] = {
        { OP_SCAN, "SCAN" }, { OP_CONST, "CONST" },
        { OP_NEG,  "NEG"  }, { OP_ABS,  "ABS"  }, { OP_NOT,  "NOT"  },
        { OP_SQRT, "SQRT" }, { OP_LOG,  "LOG"  }, { OP_EXP,  "EXP"  },
        { OP_CEIL, "CEIL" }, { OP_FLOOR,"FLOOR"}, { OP_ISNULL,"ISNULL"},
        { OP_CAST, "CAST" },
        { OP_ADD,  "ADD"  }, { OP_SUB,  "SUB"  }, { OP_MUL,  "MUL"  },
        { OP_DIV,  "DIV"  }, { OP_IDIV, "IDIV" }, { OP_MOD,  "MOD"  },
        { OP_EQ,   "EQ"   }, { OP_NE,   "NE"   }, { OP_LT,   "LT"   },
        { OP_LE,   "LE"   }, { OP_GT,   "GT"   }, { OP_GE,   "GE"   },
        { OP_AND,  "AND"  }, { OP_OR,   "OR"   },
        { OP_MIN2, "MIN2" }, { OP_MAX2, "MAX2" }, { OP_IF,   "IF"   },
        { OP_LIKE, "LIKE" }, { OP_ILIKE,"ILIKE"},
        { OP_UPPER,"UPPER"}, { OP_LOWER,"LOWER"}, { OP_STRLEN,"STRLEN"},
        { OP_SUBSTR,"SUBSTR"}, { OP_REPLACE,"REPLACE"}, { OP_TRIM,"TRIM"},
        { OP_CONCAT,"CONCAT"}, { OP_EXTRACT,"EXTRACT"},
        { OP_DATE_TRUNC,"DATE_TRUNC"},
        { OP_SUM,"SUM"}, { OP_PROD,"PROD"}, { OP_MIN,"MIN"}, { OP_MAX,"MAX"},
        { OP_COUNT,"COUNT"}, { OP_AVG,"AVG"},
        { OP_FIRST,"FIRST"}, { OP_LAST,"LAST"},
        { OP_COUNT_DISTINCT,"COUNT_DISTINCT"},
        { OP_STDDEV,"STDDEV"}, { OP_STDDEV_POP,"STDDEV_POP"},
        { OP_VAR,"VAR"}, { OP_VAR_POP,"VAR_POP"},
        { OP_PEARSON_CORR,"PEARSON_CORR"},
        { OP_MEDIAN,"MEDIAN"},
        { OP_FILTER,"FILTER"}, { OP_SORT,"SORT"}, { OP_GROUP,"GROUP"},
        { OP_PIVOT,"PIVOT"}, { OP_ANTIJOIN,"ANTIJOIN"}, { OP_JOIN,"JOIN"},
        { OP_WINDOW_JOIN,"WINDOW_JOIN"}, { OP_SELECT,"SELECT"},
        { OP_HEAD,"HEAD"}, { OP_TAIL,"TAIL"}, { OP_WINDOW,"WINDOW"},
        { OP_ALIAS,"ALIAS"}, { OP_MATERIALIZE,"MATERIALIZE"},
        { OP_EXPAND,"EXPAND"}, { OP_VAR_EXPAND,"VAR_EXPAND"},
        { OP_SHORTEST_PATH,"SHORTEST_PATH"}, { OP_WCO_JOIN,"WCO_JOIN"},
        { OP_PAGERANK,"PAGERANK"}, { OP_CONNECTED_COMP,"CONNECTED_COMP"},
        { OP_DIJKSTRA,"DIJKSTRA"}, { OP_LOUVAIN,"LOUVAIN"},
        { OP_DEGREE_CENT,"DEGREE_CENT"}, { OP_TOPSORT,"TOPSORT"},
        { OP_DFS,"DFS"}, { OP_ASTAR,"ASTAR"}, { OP_K_SHORTEST,"K_SHORTEST"},
        { OP_CLUSTER_COEFF,"CLUSTER_COEFF"}, { OP_RANDOM_WALK,"RANDOM_WALK"},
        { OP_ANN_RERANK,"ANN_RERANK"}, { OP_KNN_RERANK,"KNN_RERANK"},
    };
    size_t n = sizeof cases / sizeof cases[0];
    for (size_t i = 0; i < n; i++) {
        const char* got = ray_opcode_name(cases[i].op);
        if (strcmp(got, cases[i].name) != 0)
            FAILF("opcode %u: got \"%s\", expected \"%s\"",
                  cases[i].op, got, cases[i].name);
    }
    /* Unknown opcode -> "UNKNOWN" */
    const char* unk = ray_opcode_name(0xFFFF);
    TEST_ASSERT_STR_EQ(unk, "UNKNOWN");
    PASS();
}

/* ---------- Tests for ray_graph_dump() — header / footer ---------- */

static test_result_t test_dump_header_footer(void) {
    ray_heap_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a = ray_scan(g, "a");
    char buf[8192];
    size_t len = dump_to_buf(g, a, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    TEST_ASSERT_FMT(strstr(buf, "=== Query Plan ===")  != NULL, "missing header in dump");
    TEST_ASSERT_FMT(strstr(buf, "==================") != NULL, "missing footer in dump");
    /* SCAN annotation includes column name */
    TEST_ASSERT_FMT(strstr(buf, "SCAN(a)") != NULL, "missing SCAN(a) annotation");
    /* type_name covers RAY_I64 here (via vec scan) */
    TEST_ASSERT_FMT(strstr(buf, " -> ") != NULL, "missing type arrow");
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for OP_CONST literal-type formatting ---------- */

/* Each ray_const_* sets ext->literal with a specific atom type; dump_node's
 * inner switch on lit->type prints (i64), (%.6g), (true|false), (table), or
 * (?) for any other.  Hit them all in one graph and assert on the strings. */
static test_result_t test_dump_const_literal_types(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* I64 const */
    ray_op_t* ci = ray_const_i64(g, 12345);
    /* F64 const */
    ray_op_t* cf = ray_const_f64(g, 2.71828);
    /* BOOL const */
    ray_op_t* cb = ray_const_bool(g, true);
    ray_op_t* cb2= ray_const_bool(g, false);
    /* TABLE const — uses tbl as literal */
    ray_op_t* ct = ray_const_table(g, tbl);
    /* SYM const (RAY_SYM out_type, other lit type → falls to default "(?)") */
    ray_op_t* cs = ray_const_str(g, "x", 1);

    /* Combine into a chain so that all are reachable from one root.  We use
     * ADD nodes (binary) — the dumper only follows up to inputs[0..1], so
     * embed pairs progressively. */
    ray_op_t* p1 = ray_add(g, ci, cf);
    ray_op_t* p2 = ray_add(g, cb, cb2);
    ray_op_t* p3 = ray_add(g, p1, p2);
    ray_op_t* p4 = ray_add(g, ct, cs);
    ray_op_t* root = ray_add(g, p3, p4);

    char buf[8192];
    size_t len = dump_to_buf(g, root, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    TEST_ASSERT_FMT(strstr(buf, "(12345)")    != NULL, "missing CONST(I64)");
    TEST_ASSERT_FMT(strstr(buf, "(2.71828)")  != NULL, "missing CONST(F64)");
    TEST_ASSERT_FMT(strstr(buf, "(true)")     != NULL, "missing CONST(true)");
    TEST_ASSERT_FMT(strstr(buf, "(false)")    != NULL, "missing CONST(false)");
    TEST_ASSERT_FMT(strstr(buf, "(table)")    != NULL, "missing CONST(table)");
    TEST_ASSERT_FMT(strstr(buf, "(?)")        != NULL, "missing CONST(?) for non-{I64,F64,BOOL,TABLE}");

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for OP_JOIN annotation (INNER / LEFT / FULL) ---------- */

static test_result_t test_dump_join_annotations(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Two scan nodes used as keys (just need ray_op_t*'s). */
    ray_op_t* lk = ray_scan(g, "a");
    ray_op_t* rk = ray_scan(g, "a");
    ray_op_t* lt = ray_scan(g, "a");
    ray_op_t* rt = ray_scan(g, "a");
    ray_op_t* lk_arr[1] = { lk };
    ray_op_t* rk_arr[1] = { rk };

    /* INNER (join_type=0) */
    ray_op_t* j_in   = ray_join(g, lt, lk_arr, rt, rk_arr, 1, 0);
    /* LEFT  (join_type=1) */
    ray_op_t* j_left = ray_join(g, lt, lk_arr, rt, rk_arr, 1, 1);
    /* FULL  (join_type=2) */
    ray_op_t* j_full = ray_join(g, lt, lk_arr, rt, rk_arr, 1, 2);

    /* Combine into one root via ADDs (two-arity dumps follow inputs[0..1]). */
    ray_op_t* root = ray_add(g, ray_add(g, j_in, j_left), j_full);

    char buf[8192];
    size_t len = dump_to_buf(g, root, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    TEST_ASSERT_FMT(strstr(buf, "JOIN(INNER, keys=1)") != NULL,
                    "missing JOIN(INNER, keys=1)");
    TEST_ASSERT_FMT(strstr(buf, "JOIN(LEFT, keys=1)")  != NULL,
                    "missing JOIN(LEFT, keys=1)");
    TEST_ASSERT_FMT(strstr(buf, "JOIN(FULL, keys=1)")  != NULL,
                    "missing JOIN(FULL, keys=1)");

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for OP_GROUP annotation + recursion ---------- */

static test_result_t test_dump_group_annotation(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* GROUP keys=2, aggs=1 (sum of `a`) */
    ray_op_t* k1 = ray_scan(g, "a");
    ray_op_t* k2 = ray_scan(g, "b");
    ray_op_t* keys[2] = { k1, k2 };
    ray_op_t* a_in  = ray_scan(g, "a");
    ray_op_t* aggs_in[1] = { a_in };
    uint16_t  agg_ops[1] = { OP_SUM };
    ray_op_t* gr = ray_group(g, keys, 2, agg_ops, aggs_in, 1);
    /* ray_group sets arity=0 by default but populates inputs[0]; bump arity
     * so the OP_GROUP recursion arm in dump_node also walks standard inputs.
     * Without this the "standard inputs" loop in the OP_GROUP case is dead. */
    if (gr) gr->arity = 1;

    char buf[8192];
    size_t len = dump_to_buf(g, gr, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    TEST_ASSERT_FMT(strstr(buf, "GROUP(keys=2, aggs=1)") != NULL,
                    "missing GROUP(keys=2, aggs=1)");
    /* Recursion into keys + agg_ins (children of GROUP get printed). */
    TEST_ASSERT_FMT(strstr(buf, "SCAN(a)") != NULL, "missing SCAN(a) under GROUP");
    TEST_ASSERT_FMT(strstr(buf, "SCAN(b)") != NULL, "missing SCAN(b) under GROUP");

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for OP_HEAD / OP_TAIL annotation ---------- */

static test_result_t test_dump_head_tail_annotations(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    ray_op_t* a = ray_scan(g, "a");
    ray_op_t* h = ray_head(g, a, 7);
    ray_op_t* t = ray_tail(g, a, 3);
    ray_op_t* root = ray_add(g, h, t);

    char buf[8192];
    size_t len = dump_to_buf(g, root, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    TEST_ASSERT_FMT(strstr(buf, "HEAD(N=7)") != NULL, "missing HEAD(N=7)");
    TEST_ASSERT_FMT(strstr(buf, "TAIL(N=3)") != NULL, "missing TAIL(N=3)");

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for OP_SORT / OP_SELECT recursion into ext->sort.columns
 * and the est_rows / OP_FLAG_FUSED branches in dump_node. ---------- */

static test_result_t test_dump_sort_select_and_flags(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* SORT with a single ASC column.  `ray_sort_op` requires a table-shaped
     * input; we use an OP_SCAN as a stand-in (its out_type is the column's
     * primitive but the dumper doesn't care). */
    ray_op_t* tbl_node = ray_scan(g, "a");
    ray_op_t* sort_keys[1] = { tbl_node };
    uint8_t descs[1] = { 0 };
    ray_op_t* so = ray_sort_op(g, tbl_node, sort_keys, descs, NULL, 1);

    /* SELECT with one column */
    ray_op_t* sel_cols[1] = { tbl_node };
    ray_op_t* sl = ray_select_op(g, tbl_node, sel_cols, 1);

    /* est_rows and OP_FLAG_FUSED branches: set them on the SELECT node. */
    sl->est_rows = 99;
    sl->flags   |= OP_FLAG_FUSED;

    /* Put both under one root so a single dump walks both subtrees. */
    ray_op_t* root = ray_add(g, so, sl);

    char buf[8192];
    size_t len = dump_to_buf(g, root, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    TEST_ASSERT_FMT(strstr(buf, "SORT")   != NULL, "missing SORT");
    TEST_ASSERT_FMT(strstr(buf, "SELECT") != NULL, "missing SELECT");
    /* OP_FLAG_FUSED -> " [fused]" */
    TEST_ASSERT_FMT(strstr(buf, "[fused]") != NULL, "missing [fused] flag");
    /* est_rows formatting */
    TEST_ASSERT_FMT(strstr(buf, "~99 rows") != NULL, "missing est_rows annotation");

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for type_name() — every RAY_* tag in node->out_type ---------- */

/* The dumper's type_name() helper has one switch arm per RAY_* tag.  We hit
 * them by building one OP_SCAN per type and forcefully overwriting out_type
 * (the SCAN constructor infers from the column, but the dumper only reads
 * out_type — it doesn't validate consistency). */
static test_result_t test_dump_type_name_all(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    /* Build N scan nodes; mutate out_type to cover every type_name arm. */
    int8_t types[] = {
        RAY_LIST, RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64,
        RAY_F64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_TABLE,
        RAY_SEL, RAY_SYM,
        /* Out-of-table tag → "?" default arm */
        (int8_t)127,
    };
    int n = (int)(sizeof types / sizeof types[0]);
    ray_op_t* prev = NULL;
    for (int i = 0; i < n; i++) {
        ray_op_t* s = ray_scan(g, "a");
        s->out_type = types[i];
        if (!prev) prev = s;
        else       prev = ray_add(g, prev, s);
    }
    /* Force a known out_type on the root so the final " -> ?" is exercised. */

    char buf[16384];
    size_t len = dump_to_buf(g, prev, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    /* Spot-check a representative subset of arms. */
    const char* needles[] = {
        " -> LIST", " -> BOOL", " -> U8", " -> I16", " -> I32", " -> I64",
        " -> F64", " -> DATE", " -> TIME", " -> TIMESTAMP", " -> TABLE",
        " -> SEL", " -> SYM", " -> ?",
    };
    for (size_t i = 0; i < sizeof needles / sizeof needles[0]; i++) {
        if (!strstr(buf, needles[i])) {
            ray_graph_free(g);
            ray_release(tbl);
            ray_sym_destroy();
            ray_heap_destroy();
            FAILF("missing type arrow \"%s\"", needles[i]);
        }
    }
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for ray_graph_dump() with NULL out -> stderr fallback ---------- */

/* Pass NULL `out` and ensure no crash — the function falls back to stderr.
 * We redirect stderr to /dev/null for the duration to keep test output
 * clean.  This covers the `out ? ... : stderr` ternary. */
static test_result_t test_dump_null_out_falls_back_to_stderr(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* a = ray_scan(g, "a");

    /* Redirect stderr to /dev/null so the dump output doesn't pollute the
     * test runner.  Restored afterwards. */
    fflush(stderr);
    int saved = dup(fileno(stderr));
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0 && saved >= 0) {
        dup2(devnull, fileno(stderr));
    }

    ray_graph_dump(g, a, NULL);  /* must not crash */

    fflush(stderr);
    if (saved >= 0) {
        dup2(saved, fileno(stderr));
        close(saved);
    }
    if (devnull >= 0) close(devnull);

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Tests for ray_graph_dump() with NULL root -> early return ---------- */

/* dump_node guards against NULL — exercise that branch explicitly. */
static test_result_t test_dump_null_root(void) {
    ray_heap_init();
    (void)ray_sym_init();
    ray_t* tbl = make_tiny_table();
    ray_graph_t* g = ray_graph_new(tbl);

    char buf[1024];
    size_t len = dump_to_buf(g, NULL, buf, sizeof buf);
    TEST_ASSERT_FMT(len > 0, "empty dump output");
    /* With NULL root the dumper still emits header + footer, no body. */
    TEST_ASSERT_FMT(strstr(buf, "=== Query Plan ===")  != NULL, "missing header");
    TEST_ASSERT_FMT(strstr(buf, "==================") != NULL, "missing footer");

    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
    PASS();
}

/* ---------- Registration ---------- */

const test_entry_t dump_entries[] = {
    { "dump/opcode_name_all",           test_dump_opcode_name_all,           NULL, NULL },
    { "dump/header_footer",             test_dump_header_footer,             NULL, NULL },
    { "dump/const_literal_types",       test_dump_const_literal_types,       NULL, NULL },
    { "dump/join_annotations",          test_dump_join_annotations,          NULL, NULL },
    { "dump/group_annotation",          test_dump_group_annotation,          NULL, NULL },
    { "dump/head_tail_annotations",     test_dump_head_tail_annotations,     NULL, NULL },
    { "dump/sort_select_and_flags",     test_dump_sort_select_and_flags,     NULL, NULL },
    { "dump/type_name_all",             test_dump_type_name_all,             NULL, NULL },
    { "dump/null_out_falls_back",       test_dump_null_out_falls_back_to_stderr, NULL, NULL },
    { "dump/null_root",                 test_dump_null_root,                 NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
