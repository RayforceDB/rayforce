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

/* The journal's physical records: every way a global changes must come
 * back, the same value, in a process that only applies the log.
 *
 * Each case runs its statements with a journal open (one ray_eval_str is
 * one commit), formats what a probe expression reads, destroys the
 * runtime, recovers into a fresh one and compares the probe again. */

#define _GNU_SOURCE
#include "test.h"
#include <rayforce.h>
#include "store/journal.h"
#include "store/serde.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "lang/format.h"
#include "core/runtime.h"
#include "mem/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void wal_setup(void)    { ray_runtime_create(0, NULL); }
static void wal_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static void wal_base(char* base, size_t sz, const char* tag) {
    snprintf(base, sz, "/tmp/wal_test_%s_XXXXXX", tag);
    int fd = mkstemp(base);
    if (fd >= 0) { close(fd); unlink(base); }
}

static void wal_cleanup(const char* base) {
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -f %s.log %s.qdb %s.lk %s.*.log", base, base, base, base);
    if (system(cmd) != 0) { /* best effort */ }
}

static bool wal_run(const char* src) {
    ray_t* r = ray_eval_str(src);
    if (!r) return true;
    if (RAY_IS_ERR(r)) { ray_error_free(r); return false; }
    if (r != RAY_NULL_OBJ) ray_release(r);
    return true;
}

/* What `probe` reads, in the repr the rfl runner compares values by
 * (attrs that are only hints, such as HAS_NULLS on a column without a
 * null, are not part of a value); NULL when it raises. */
static uint8_t* wal_probe(const char* probe, int64_t* len) {
    *len = 0;
    ray_t* v = ray_eval_str(probe);
    if (!v || RAY_IS_ERR(v)) { if (v) ray_error_free(v); return NULL; }
    ray_t* str = ray_fmt(v, 0);
    if (v != RAY_NULL_OBJ) ray_release(v);
    if (!str || RAY_IS_ERR(str)) return NULL;
    int64_t n = (int64_t)ray_str_len(str);
    uint8_t* buf = (uint8_t*)ray_sys_alloc((size_t)n + 1);
    if (buf) { memcpy(buf, ray_str_ptr(str), (size_t)n); *len = n; }
    ray_release(str);
    return buf;
}

static int64_t wal_file_size(const char* base) {
    char path[300];
    snprintf(path, sizeof(path), "%s.log", base);
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_size : -1;
}

/* Run `ops` (statements separated by '\n', one commit each) under a
 * journal, then recover in a fresh runtime: `probe` must read the same
 * value.  0 on success, else the step that failed. */
static int wal_roundtrip(const char* ops, const char* probe) {
    char base[256];
    wal_base(base, sizeof(base), "rt");
    int fail = 0;
    uint8_t *want = NULL, *got = NULL;
    int64_t nwant = 0, ngot = 0;

    wal_teardown();                                /* nothing the log does not hold */
    wal_setup();
    if (ray_journal_open(base, RAY_JOURNAL_ASYNC) != RAY_OK) { fail = 1; goto done; }
    char* copy = strdup(ops);
    for (char* line = strtok(copy, "\n"); line && !fail; line = strtok(NULL, "\n"))
        if (!wal_run(line)) fail = 2;
    free(copy);
    if (!fail) {
        want = wal_probe(probe, &nwant);
        if (!want) fail = 3;
    }
    if (ray_journal_close() != RAY_OK && !fail) fail = 4;
    if (fail) goto done;

    wal_teardown();                                /* the process is gone */
    wal_setup();
    got = wal_probe(probe, &ngot);                 /* and so is what it held */
    if (got && ngot == nwant && memcmp(got, want, (size_t)nwant) == 0) { fail = 8; goto done; }
    if (got) { ray_sys_free(got); got = NULL; }
    if (ray_journal_recover(base) != RAY_OK) { fail = 5; goto done; }
    got = wal_probe(probe, &ngot);
    if (!got) fail = 6;
    else if (ngot != nwant || memcmp(got, want, (size_t)nwant) != 0) fail = 7;

done:
    if (ray_journal_is_open()) ray_journal_close();
    if (want) ray_sys_free(want);
    if (got) ray_sys_free(got);
    wal_cleanup(base);
    return fail;
}

/* The comparison these tests rest on sees a table's cells, not its shape. */
static test_result_t test_wal_probe_sees_content(void) {
    int64_t na = 0, nb = 0;
    uint8_t* a = wal_probe("(table [k s] (list [1 2] ['a 'b]))", &na);
    uint8_t* b = wal_probe("(table [k s] (list [1 2] ['a 'c]))", &nb);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    bool differ = na != nb || memcmp(a, b, (size_t)na) != 0;
    ray_sys_free(a);
    ray_sys_free(b);
    TEST_ASSERT_TRUE(differ);
    PASS();
}

/* ── every path that changes a global ─────────────────────────────── */

#define T3 "(table [k v s] (list [1 2 3] [10 20 30] ['a 'b 'c]))"

static const struct { const char* name; const char* ops; const char* probe; } wal_cases[] = {
    { "set atom",        "(set x 42)", "x" },
    { "set vector",      "(set x [1 2 3])", "x" },
    { "set list",        "(set x (list 1 \"two\" 'three [4.0 5.0]))", "x" },
    { "set dict",        "(set x (dict ['a 'b] (list 1 [2 3])))", "x" },
    { "set table",       "(set t " T3 ")", "t" },
    { "set lambda",      "(set y 5)\n(set f (fn [a] (+ a y)))", "(list f (f 1))" },
    { "set twice",       "(set x 1)\n(set x 2)", "x" },
    { "two in a commit", "(do (set x 1) (set y [2 3]))", "(list x y)" },
    { "del",             "(set x 1)\n(set y 2)\n(del x)", "(list y (try x (fn [e] 'unbound)))" },
    { "set from fn",     "(set f (fn [] (set g 5)))\n(f)", "g" },
    /* #1006: data whose head names a builtin stays data */
    { "list with a builtin head", "(set x (list 'count [1 2 3]))", "x" },
    /* settled once: replay must not draw again */
    { "rand",            "(set r (rand 20 1000000))", "r" },

    { "dotted set",      "(set ns.a 1)\n(set ns.b.c [1 2])", "(list ns.a ns.b.c)" },
    { "dotted table",    "(set ns.t " T3 ")", "ns.t" },
    { "dotted del",      "(set ns.a 1)\n(set ns.b 2)\n(del ns.a)", "ns" },

    { "insert row",      "(set t " T3 ")\n(insert 't (list 4 40 'd))", "t" },
    { "insert rows",     "(set t " T3 ")\n(insert 't (table [k v s] (list [4 5] [40 50] ['d 'e])))", "t" },
    { "insert many",     "(set t " T3 ")\n(insert 't (list 4 40 'd))\n(insert 't (list 5 50 'e))\n(insert 't (list 6 60 'f))", "t" },
    { "insert shared",   "(set t " T3 ")\n(set u t)\n(insert 't (list 4 40 'd))", "(list t u)" },
    { "insert dotted",   "(set ns.t " T3 ")\n(insert 'ns.t (list 4 40 'd))", "ns.t" },
    { "insert vector",   "(set v [1 2 3])\n(insert 'v 4)", "v" },
    { "insert list",     "(set l (list 1 \"a\"))\n(insert 'l 'x)", "l" },
    { "insert in a fn",  "(set t " T3 ")\n(set f (fn [] (insert 't (list 4 40 'd))))\n(f)", "t" },
    { "insert nulls",    "(set t " T3 ")\n(insert 't (list 0Nl 40 'd))\n(insert 't (list 5 0Nl 'e))", "(list t (nil? (at t 'k)) (nil? (at t 'v)))" },
    { "insert new sym",  "(set t " T3 ")\n(insert 't (list 4 40 'never_seen_before_sym))", "t" },

    { "upsert new",      "(set t " T3 ")\n(upsert 't 1 (list 4 40 'd))", "t" },
    { "upsert existing", "(set t " T3 ")\n(upsert 't 1 (list 2 77 'z))", "t" },
    { "upsert twice",    "(set t " T3 ")\n(upsert 't 1 (list 2 77 'z))\n(upsert 't 1 (list 9 99 'y))", "t" },
    { "upsert shared",   "(set t " T3 ")\n(set u t)\n(upsert 't 1 (list 2 77 'z))", "(list t u)" },
    { "upsert dotted",   "(set ns.t " T3 ")\n(upsert 'ns.t 1 (list 2 77 'z))", "ns.t" },

    { "update where",    "(set t " T3 ")\n(update {from: 't v: 99 where: (== k 2)})", "t" },
    { "update all",      "(set t " T3 ")\n(update {from: 't v: (* v 2)})", "t" },
    { "update by",       "(set t " T3 ")\n(update {from: 't tot: (sum v) by: s})", "t" },
    { "update shared",   "(set t " T3 ")\n(set u t)\n(update {from: 't v: 99 where: (== k 2)})", "(list t u)" },
    { "update dotted",   "(set ns.t " T3 ")\n(update {from: 'ns.t v: 99 where: (== k 2)})", "ns.t" },

    { "delete where",    "(set t " T3 ")\n(delete {from: 't where: (> k 1)})", "t" },
    { "delete shared",   "(set t " T3 ")\n(set u t)\n(delete {from: 't where: (> k 1)})", "(list t u)" },
    { "delete dotted",   "(set ns.t " T3 ")\n(delete {from: 'ns.t where: (== k 2)})", "ns.t" },

    { "alter set",       "(set v [1 2 3])\n(alter 'v set 0 9)", "v" },
    { "alter concat",    "(set v [1 2 3])\n(alter 'v concat [4 5])", "v" },
    { "alter dotted",    "(set ns.v [1 2 3])\n(alter 'ns.v set 1 9)", "ns.v" },

    { "mixed",           "(set t " T3 ")\n(insert 't (list 4 40 'd))\n(update {from: 't v: 0 where: (== k 4)})\n"
                         "(insert 't (list 5 50 'e))\n(delete {from: 't where: (== k 1)})\n(insert 't (list 6 60 'f))", "t" },
};

static test_result_t test_wal_roundtrip_paths(void) {
    for (size_t i = 0; i < sizeof(wal_cases) / sizeof(wal_cases[0]); i++) {
        int fail = wal_roundtrip(wal_cases[i].ops, wal_cases[i].probe);
        if (fail) {
            static char msg[160];
            snprintf(msg, sizeof(msg), "case '%s' failed at step %d", wal_cases[i].name, fail);
            FAIL(msg);
        }
    }
    PASS();
}

/* ── transactions: the undo log ───────────────────────────────────── */

/* Every path again, this time undone: build the case's state, then inside
 * a transaction run its mutating statements once more (they now start
 * from that state) and bind a new name; rollback must leave what the
 * probe read before, and nothing of the new name. */
static test_result_t test_tx_rollback_paths(void) {
    for (size_t i = 0; i < sizeof(wal_cases) / sizeof(wal_cases[0]); i++) {
        wal_teardown();
        wal_setup();
        int fail = 0;
        char* copy = strdup(wal_cases[i].ops);
        char* rest = strchr(copy, '\n');           /* past the statement that creates the state */
        for (char* line = strtok(copy, "\n"); line && !fail; line = strtok(NULL, "\n"))
            if (!wal_run(line)) fail = 1;
        free(copy);
        int64_t nwant = 0, ngot = 0;
        uint8_t* want = fail ? NULL : wal_probe(wal_cases[i].probe, &nwant);
        if (!fail && !want) fail = 2;

        if (!fail && ray_tx_begin(RAY_TX_LOCAL, 0, 0) != RAY_OK) fail = 3;
        if (!fail) {
            copy = strdup(wal_cases[i].ops);
            rest = strchr(copy, '\n');
            for (char* line = rest ? strtok(rest + 1, "\n") : NULL; line; line = strtok(NULL, "\n"))
                (void)wal_run(line);
            free(copy);
            if (!wal_run("(set tx_new_name [1 2 3])")) fail = 4;
            if (!wal_run("(set tx_ns.inner 1)")) fail = 4;
            ray_tx_rollback();
            if (ray_tx_active) fail = 5;
        }
        uint8_t* got = fail ? NULL : wal_probe(wal_cases[i].probe, &ngot);
        if (!fail && !got) fail = 6;
        if (!fail && (ngot != nwant || memcmp(got, want, (size_t)nwant) != 0)) fail = 7;
        if (!fail && (wal_run("tx_new_name") || wal_run("tx_ns"))) fail = 8;
        if (want) ray_sys_free(want);
        if (got) ray_sys_free(got);
        if (fail) {
            static char msg[160];
            snprintf(msg, sizeof(msg), "case '%s' failed at step %d", wal_cases[i].name, fail);
            FAIL(msg);
        }
    }
    PASS();
}

/* A value somebody else also holds is not written through: neither by the
 * transaction's writes nor by the rollback. */
static test_result_t test_tx_leaves_aliases_alone(void) {
    TEST_ASSERT_TRUE(wal_run("(set t " T3 ")"));
    TEST_ASSERT_TRUE(wal_run("(set ns.t " T3 ")"));
    TEST_ASSERT_TRUE(wal_run("(set alias t)"));
    TEST_ASSERT_TRUE(wal_run("(set nsalias ns)"));
    int64_t nwant = 0, ngot = 0;
    uint8_t* want = wal_probe("(list alias nsalias t ns)", &nwant);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(insert 't (list 4 40 'd))"));
    TEST_ASSERT_TRUE(wal_run("(update {from: 't v: 0 where: (== k 1)})"));
    TEST_ASSERT_TRUE(wal_run("(upsert 'ns.t 1 (list 2 77 'z))"));
    TEST_ASSERT_TRUE(wal_run("(delete {from: 'ns.t where: (== k 3)})"));
    TEST_ASSERT_TRUE(wal_run("(insert 'ns.t (list 9 90 'q))"));
    int64_t nmid = 0;
    uint8_t* mid = wal_probe("(list alias nsalias)", &nmid);
    ray_tx_rollback();
    uint8_t* got = wal_probe("(list alias nsalias t ns)", &ngot);
    TEST_ASSERT_NOT_NULL(got);
    bool same = ngot == nwant && memcmp(got, want, (size_t)nwant) == 0;
    uint8_t* aliases = wal_probe("(list alias nsalias)", &ngot);
    bool untouched = mid && aliases && ngot == nmid && memcmp(mid, aliases, (size_t)nmid) == 0;
    ray_sys_free(want); ray_sys_free(got);
    if (mid) ray_sys_free(mid);
    if (aliases) ray_sys_free(aliases);
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_TRUE(untouched);
    PASS();
}

/* Created, changed and deleted in one transaction, each undone; and
 * several changes of one name go back to what it held before the first. */
static test_result_t test_tx_rollback_create_change_delete(void) {
    TEST_ASSERT_TRUE(wal_run("(set keep 1)"));
    TEST_ASSERT_TRUE(wal_run("(set gone [1 2 3])"));
    TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set keep 2)"));
    TEST_ASSERT_TRUE(wal_run("(set keep 3)"));
    TEST_ASSERT_TRUE(wal_run("(del keep)"));
    TEST_ASSERT_TRUE(wal_run("(set keep 4)"));
    TEST_ASSERT_TRUE(wal_run("(del gone)"));
    TEST_ASSERT_TRUE(wal_run("(set made 5)"));
    TEST_ASSERT_TRUE(wal_run("(del made)"));
    TEST_ASSERT_TRUE(wal_run("(set made 6)"));
    ray_tx_rollback();
    int64_t nwant = 0, ngot = 0;
    uint8_t* want = wal_probe("(list 1 [1 2 3] 'unbound)", &nwant);
    uint8_t* got  = wal_probe("(list keep gone (try made (fn [e] 'unbound)))", &ngot);
    bool same = want && got && ngot == nwant && memcmp(got, want, (size_t)nwant) == 0;
    if (want) ray_sys_free(want);
    if (got) ray_sys_free(got);
    TEST_ASSERT_TRUE(same);
    PASS();
}

/* The undo log holds one reference to each old value and gives every one
 * back: after either ending the binding is the table's only owner again,
 * which is what lets the next insert append in place. */
static test_result_t test_tx_gives_references_back(void) {
    TEST_ASSERT_TRUE(wal_run("(set t " T3 ")"));
    int64_t sym = ray_sym_intern("t", 1);
    TEST_ASSERT_EQ_I(ray_env_get_global(sym)->rc, 1);
    for (int commit = 0; commit < 2; commit++) {
        TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_OK);
        TEST_ASSERT_TRUE(wal_run("(insert 't (list 4 40 'd))"));
        TEST_ASSERT_TRUE(wal_run("(insert 't (list 5 50 'e))"));
        TEST_ASSERT_TRUE(wal_run("(update {from: 't v: 0 where: (== k 1)})"));
        if (commit) TEST_ASSERT_EQ_I(ray_tx_commit(), RAY_OK);
        else ray_tx_rollback();
        TEST_ASSERT_FALSE(ray_tx_active);
        TEST_ASSERT_EQ_I(ray_env_get_global(sym)->rc, 1);
    }
    PASS();
}

/* One transaction is one journal commit; a rolled back one is none. */
static test_result_t test_tx_is_one_commit(void) {
    char base[256], path[300];
    wal_base(base, sizeof(base), "txc");
    snprintf(path, sizeof(path), "%s.log", base);
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set t " T3 ")"));
    int64_t chunks0 = 0, bytes0 = 0, chunks = 0, bytes = 0;
    TEST_ASSERT_EQ_I(ray_journal_validate(path, &chunks0, &bytes0), RAY_OK);

    TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(insert 't (list 4 40 'd))"));
    TEST_ASSERT_TRUE(wal_run("(set a 1)"));
    TEST_ASSERT_TRUE(wal_run("(set b 2)"));
    TEST_ASSERT_EQ_I(ray_journal_validate(path, &chunks, &bytes), RAY_OK);
    TEST_ASSERT_EQ_I(bytes, bytes0);               /* nothing reaches the log before the commit */
    ray_tx_rollback();
    TEST_ASSERT_TRUE(wal_run("(sum [1 2])"));      /* an evaluation boundary after it */
    TEST_ASSERT_EQ_I(ray_journal_validate(path, &chunks, &bytes), RAY_OK);
    TEST_ASSERT_EQ_I(bytes, bytes0);

    TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(insert 't (list 5 50 'e))"));
    TEST_ASSERT_TRUE(wal_run("(set a 1)"));
    TEST_ASSERT_TRUE(wal_run("(set b 2)"));
    TEST_ASSERT_EQ_I(ray_tx_commit(), RAY_OK);
    TEST_ASSERT_EQ_I(ray_journal_validate(path, &chunks, &bytes), RAY_OK);
    TEST_ASSERT_EQ_I(chunks, chunks0 + 1);

    int64_t nwant = 0, ngot = 0;
    uint8_t* want = wal_probe("(list t a b)", &nwant);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    wal_teardown();
    wal_setup();
    TEST_ASSERT_EQ_I(ray_journal_recover(base), RAY_OK);
    uint8_t* got = wal_probe("(list t a b)", &ngot);
    bool same = want && got && ngot == nwant && memcmp(got, want, (size_t)nwant) == 0;
    if (want) ray_sys_free(want);
    if (got) ray_sys_free(got);
    wal_cleanup(base);
    TEST_ASSERT_TRUE(same);
    PASS();
}

/* Only one at a time, and the ends need one open. */
static test_result_t test_tx_begin_commit_rules(void) {
    TEST_ASSERT_EQ_I(ray_tx_commit(), RAY_ERR_DOMAIN);
    ray_tx_rollback();                             /* no-op */
    TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_OK);
    TEST_ASSERT_EQ_I(ray_tx_begin(RAY_TX_LOCAL, 0, 0), RAY_ERR_DOMAIN);
    TEST_ASSERT_TRUE(ray_tx_active);               /* the refused begin did not end it */
    TEST_ASSERT_EQ_I(ray_tx_left_ms(), INT64_MAX);
    TEST_ASSERT_EQ_I(ray_tx_commit(), RAY_OK);
    TEST_ASSERT_EQ_I(ray_tx_begin(7, 50, 1000), RAY_OK);
    TEST_ASSERT_EQ_I(ray_tx_owner(), 7);
    TEST_ASSERT_TRUE(ray_tx_left_ms() > 0 && ray_tx_left_ms() <= 50);
    ray_tx_rollback();
    TEST_ASSERT_FALSE(ray_tx_active);
    PASS();
}

/* An insert is logged as the rows it adds: the commit of one row into a
 * large table must not grow with the table. */
static test_result_t test_wal_insert_logs_rows_not_table(void) {
    char base[256];
    wal_base(base, sizeof(base), "rows");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set big (table [k v] (list (til 100000) (til 100000))))"));
    int64_t before = wal_file_size(base);
    TEST_ASSERT_TRUE(before > 100000);             /* the table itself was logged whole */
    TEST_ASSERT_TRUE(wal_run("(insert 'big (list 7 7))"));
    int64_t one = wal_file_size(base) - before;
    TEST_ASSERT_TRUE(wal_run("(insert 'big (list 8 8))"));
    int64_t two = wal_file_size(base) - before - one;
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    wal_cleanup(base);
    TEST_ASSERT_TRUE(one > 0 && one < 512);
    TEST_ASSERT_EQ_I(two, one);
    PASS();
}

/* A statement that changes nothing writes nothing. */
static test_result_t test_wal_read_writes_no_frame(void) {
    char base[256];
    wal_base(base, sizeof(base), "ro");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set x [1 2 3])"));
    int64_t before = wal_file_size(base);
    TEST_ASSERT_TRUE(wal_run("(sum x)"));
    TEST_ASSERT_TRUE(wal_run("(insert x 4)"));     /* functional: no global changes */
    TEST_ASSERT_TRUE(!wal_run("(nosuchfn 1)"));
    int64_t after = wal_file_size(base);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    wal_cleanup(base);
    TEST_ASSERT_EQ_I(after, before);
    PASS();
}

/* A statement that fails half way: what it changed before failing is in
 * memory, so it is in the log too. */
static test_result_t test_wal_failed_statement_logs_what_it_changed(void) {
    TEST_ASSERT_EQ_I(wal_roundtrip("(set x 1)\n(try (do (set x 2) (nosuchfn 1)) (fn [e] 0))", "x"), 0);
    PASS();
}

/* A value serde cannot write is refused before it is bound. */
static test_result_t test_wal_unloggable_value_is_refused(void) {
    char base[256];
    wal_base(base, sizeof(base), "refuse");
    TEST_ASSERT_TRUE(wal_run("(set e (table [src dst w] (list [0 1] [1 2] [1.0 2.0])))"));
    TEST_ASSERT_TRUE(wal_run("(set g0 (.graph.build e 'src 'dst 'w))"));   /* no journal: fine */
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    int64_t before = wal_file_size(base);
    TEST_ASSERT_TRUE(!wal_run("(set g1 (.graph.build e 'src 'dst 'w))"));
    TEST_ASSERT_TRUE(!wal_run("g1"));              /* not bound */
    TEST_ASSERT_TRUE(wal_run("(set ok 1)"));       /* and the log is not stale */
    TEST_ASSERT_TRUE(wal_file_size(base) > before);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    wal_cleanup(base);
    PASS();
}

/* Cut the log at every byte of its last commit: recovery drops exactly
 * that commit, keeps the ones before it, and leaves a log that the next
 * commit extends cleanly. */
static test_result_t test_wal_torn_commit_at_every_byte(void) {
    char base[256], path[300];
    wal_base(base, sizeof(base), "torn");
    snprintf(path, sizeof(path), "%s.log", base);

    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set keep [1 2 3])"));
    int64_t good = wal_file_size(base);
    TEST_ASSERT_TRUE(wal_run("(set lost (table [a] (list [1 2 3 4])))"));
    int64_t full = wal_file_size(base);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    TEST_ASSERT_TRUE(good > 0 && full > good);

    uint8_t* image = (uint8_t*)ray_sys_alloc((size_t)full);
    TEST_ASSERT_NOT_NULL(image);
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQ_I((int64_t)fread(image, 1, (size_t)full, f), full);
    fclose(f);

    int bad = 0;
    for (int64_t cut = good + 1; cut < full && !bad; cut++) {
        f = fopen(path, "wb");
        if (!f || fwrite(image, 1, (size_t)cut, f) != (size_t)cut) bad = 1;
        if (f) fclose(f);
        wal_teardown();
        wal_setup();
        if (!bad && ray_journal_open(base, RAY_JOURNAL_ASYNC) != RAY_OK) bad = 2;
        if (!bad && wal_file_size(base) != good) bad = 3;       /* cut back to the last commit */
        if (!bad && !wal_run("(sum keep)")) bad = 4;
        if (!bad && wal_run("lost")) bad = 5;                   /* never applied */
        if (!bad && !wal_run("(set after 9)")) bad = 6;
        if (ray_journal_is_open() && ray_journal_close() != RAY_OK && !bad) bad = 7;
        wal_teardown();
        wal_setup();
        if (!bad && ray_journal_recover(base) != RAY_OK) bad = 8;   /* the appended commit reads back */
        if (!bad && !wal_run("(+ after (sum keep))")) bad = 9;
    }
    ray_sys_free(image);
    wal_cleanup(base);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
}

/* Snapshot plus the tail written after it is the same state as the whole log. */
static test_result_t test_wal_snapshot_plus_tail(void) {
    char base[256];
    wal_base(base, sizeof(base), "snap");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set t " T3 ")"));
    TEST_ASSERT_TRUE(wal_run("(insert 't (list 4 40 'd))"));
    TEST_ASSERT_EQ_I(ray_journal_snapshot(), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(insert 't (list 5 50 'e))"));
    TEST_ASSERT_TRUE(wal_run("(set after [1 2])"));
    int64_t nwant = 0, ngot = 0;
    uint8_t* want = wal_probe("(list t after)", &nwant);
    TEST_ASSERT_NOT_NULL(want);
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);

    wal_teardown();
    wal_setup();
    TEST_ASSERT_EQ_I(ray_journal_recover(base), RAY_OK);
    uint8_t* got = wal_probe("(list t after)", &ngot);
    TEST_ASSERT_NOT_NULL(got);
    bool same = ngot == nwant && memcmp(got, want, (size_t)nwant) == 0;
    ray_sys_free(want);
    ray_sys_free(got);
    wal_cleanup(base);
    TEST_ASSERT_TRUE(same);
    PASS();
}

/* Changes buffered when a snapshot is taken belong before it: they must
 * not land on the fresh log and apply a second time. */
static test_result_t test_wal_snapshot_inside_a_commit(void) {
    TEST_ASSERT_EQ_I(wal_roundtrip("(set t " T3 ")\n(do (insert 't (list 4 40 'd)) (.log.snapshot) (insert 't (list 5 50 'e)))",
                                   "t"), 0);
    PASS();
}

/* Recovery applies; it does not log what it applies. */
static test_result_t test_wal_recovery_is_not_logged(void) {
    char base[256];
    wal_base(base, sizeof(base), "norelog");
    TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
    TEST_ASSERT_TRUE(wal_run("(set x [1 2 3])"));
    TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
    int64_t size = wal_file_size(base);
    for (int i = 0; i < 3; i++) {                  /* three restarts */
        wal_teardown();
        wal_setup();
        TEST_ASSERT_EQ_I(ray_journal_open(base, RAY_JOURNAL_ASYNC), RAY_OK);
        TEST_ASSERT_TRUE(wal_run("(sum x)"));
        TEST_ASSERT_EQ_I(ray_journal_close(), RAY_OK);
        TEST_ASSERT_EQ_I(wal_file_size(base), size);
    }
    wal_cleanup(base);
    PASS();
}

const test_entry_t wal_entries[] = {
    { "wal/probe_sees_content",     test_wal_probe_sees_content,     wal_setup, wal_teardown },
    { "wal/roundtrip_paths",        test_wal_roundtrip_paths,        wal_setup, wal_teardown },
    { "wal/insert_logs_rows",       test_wal_insert_logs_rows_not_table, wal_setup, wal_teardown },
    { "wal/read_writes_no_frame",   test_wal_read_writes_no_frame,   wal_setup, wal_teardown },
    { "wal/failed_statement",       test_wal_failed_statement_logs_what_it_changed, wal_setup, wal_teardown },
    { "wal/unloggable_refused",     test_wal_unloggable_value_is_refused, wal_setup, wal_teardown },
    { "wal/torn_commit_every_byte", test_wal_torn_commit_at_every_byte, wal_setup, wal_teardown },
    { "wal/snapshot_plus_tail",     test_wal_snapshot_plus_tail,     wal_setup, wal_teardown },
    { "wal/snapshot_inside_commit", test_wal_snapshot_inside_a_commit, wal_setup, wal_teardown },
    { "wal/recovery_not_logged",    test_wal_recovery_is_not_logged, wal_setup, wal_teardown },
    { "tx/rollback_paths",          test_tx_rollback_paths,          wal_setup, wal_teardown },
    { "tx/leaves_aliases_alone",    test_tx_leaves_aliases_alone,    wal_setup, wal_teardown },
    { "tx/rollback_create_change_delete", test_tx_rollback_create_change_delete, wal_setup, wal_teardown },
    { "tx/gives_references_back",   test_tx_gives_references_back,   wal_setup, wal_teardown },
    { "tx/is_one_commit",           test_tx_is_one_commit,           wal_setup, wal_teardown },
    { "tx/begin_commit_rules",      test_tx_begin_commit_rules,      wal_setup, wal_teardown },
    { NULL, NULL, NULL, NULL },
};
