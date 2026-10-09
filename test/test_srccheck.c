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

#include "test.h"
#include "table/table.h"
#include "vec/vec.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Line number of the first occurrence of any needle (0 if none). */
static int first_hit(const char* path, const char* const* needles) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char line[4096]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        for (int i = 0; needles[i]; i++)
            if (strstr(line, needles[i])) { fclose(f); return ln; }
    }
    fclose(f);
    return 0;
}

typedef struct { char bad[512]; int nbad; int nseen; bool saw_table; } srccheck_acc_t;

static void walk(const char* dir, const char* rel, srccheck_acc_t* acc,
                 void (*fn)(const char*, const char*, srccheck_acc_t*)) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024], r[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s/%s", rel, e->d_name);
        struct stat st;
        if (stat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk(p, r, acc, fn); continue; }
        size_t n = strlen(p);
        if (n > 2 && (strcmp(p + n - 2, ".c") == 0 || strcmp(p + n - 2, ".h") == 0)) fn(p, r, acc);
    }
    closedir(d);
}

static void check_getters(const char* p, const char* rel, srccheck_acc_t* acc) {
    static const char* const needles[] = { "ray_table_get_col(", "ray_table_get_col_idx(", NULL };
    acc->nseen++;
    if (strcmp(rel, "src/table/table.c") == 0) { acc->saw_table = true; return; }
    int ln = first_hit(p, needles);
    if (ln) {
        if (!acc->nbad) snprintf(acc->bad, sizeof acc->bad, "%s:%d uses a legacy getter", rel, ln);
        acc->nbad++;
    }
}

/* The suite runs from the repository root; accept a build directory one level
 * below it as well. */
static test_result_t test_no_legacy_getters(void) {
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char dir[64]; snprintf(dir, sizeof dir, "%s/src", root);
    srccheck_acc_t acc = { "", 0, 0, false };
    walk(dir, "src", &acc, check_getters);
    TEST_ASSERT_FMT(acc.saw_table && acc.nseen > 50, "source tree not scanned (%d files)", acc.nseen);
    TEST_ASSERT_FMT(acc.nbad == 0, "%s (%d files)", acc.bad, acc.nbad);
    PASS();
}

/* Every [1] slot read outside table.c is marked: a dict / lambda / list /
 * MAPCOMMON slot (not-a-table), or a table slot through which no column of
 * an encoded table is read: an ownership edge, or a table proven to hold no
 * encoded slot (plain-table). */
static bool raw_list_hit(const char* line) {
    if (strstr(line, "slots[1]")) return true;
    const char* p = line;
    while ((p = strstr(p, "(ray_t**)ray_data(")) != NULL) {
        const char* q = strchr(p, ')');                 /* closes the cast */
        q = q ? strchr(q + 1, ')') : NULL;              /* closes ray_data( */
        if (q && q[1] == ')' && q[2] == '[' && q[3] == '1' && q[4] == ']') return true;
        p++;
    }
    return false;
}

static void check_raw_list(const char* p, const char* rel, srccheck_acc_t* acc) {
    if (strcmp(rel, "src/table/table.c") == 0) return;
    FILE* f = fopen(p, "rb");
    if (!f) return;
    char line[4096]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (!raw_list_hit(line)) continue;
        if (strstr(line, "/* not-a-table */") || strstr(line, "/* plain-table */")) continue;
        if (!acc->nbad) snprintf(acc->bad, sizeof acc->bad, "%s:%d reads slot [1] unmarked", rel, ln);
        acc->nbad++;
    }
    fclose(f);
}

static test_result_t test_no_raw_column_list(void) {
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char dir[64]; snprintf(dir, sizeof dir, "%s/src", root);
    srccheck_acc_t acc = { "", 0, 0, false };
    walk(dir, "src", &acc, check_raw_list);
    TEST_ASSERT_FMT(acc.nbad == 0, "%s (%d sites)", acc.bad, acc.nbad);
    PASS();
}

/* The generation-2 materializer attaches through the persisted-attach
 * predicate only (spec "Source checks"). */
static test_result_t test_materializer_attach(void) {
    static const char* const needles[] = { "ray_index_attach_built", NULL };
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char path[128]; snprintf(path, sizeof path, "%s/src/store/enccol.c", root);
    TEST_ASSERT_EQ_I(first_hit(path, needles), 0);
    PASS();
}

static test_result_t test_cols_inplace_plain(void) {
    TEST_ASSERT_TRUE(ray_table_cols_inplace(NULL) == NULL);
    ray_t* v = ray_vec_new(RAY_I64, 0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(v));
    TEST_ASSERT_TRUE(ray_table_cols_inplace(v) == NULL);   /* not a table */
    ray_release(v);
    ray_t* t = ray_table_new(0);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    ray_t* c = ray_table_cols_inplace(t);
    TEST_ASSERT_TRUE(c != NULL && !RAY_IS_ERR(c));
    ray_release(t);
    PASS();
}

/* Every "/tmp" path a test names lives under this process's own root, so
 * two suites on one host never share a file: plain paths, paths inside a
 * Rayfall or shell line, the bare root; an already-moved path and a path
 * that merely contains "/tmp" elsewhere are left alone.  The root exists. */
static test_result_t test_tmp_paths_per_process(void) {
    char root[64];
    snprintf(root, sizeof root, "/tmp/rayforce_t.%ld", (long)getpid());
    char want[256];
    snprintf(want, sizeof want, "%s/rayforce_x/a", root);
    TEST_ASSERT_STR_EQ(ray_test_tmp("/tmp/rayforce_x/a"), want);
    TEST_ASSERT_STR_EQ(ray_test_tmp(want), want);
    snprintf(want, sizeof want, "(.db.splayed.set \"%s/t\" T) rm -rf %s/u; cd %s", root, root, root);
    TEST_ASSERT_STR_EQ(ray_test_tmp("(.db.splayed.set \"/tmp/t\" T) rm -rf /tmp/u; cd /tmp"), want);
    TEST_ASSERT_STR_EQ(ray_test_tmp("/var/tmp/x /tmpx rf_tmp/y"), "/var/tmp/x /tmpx rf_tmp/y");
    TEST_ASSERT_TRUE(ray_test_tmp("/tmp/rayforce_x/a") == ray_test_tmp("/tmp/rayforce_x/a"));
    struct stat st;
    TEST_ASSERT_TRUE(stat(root, &st) == 0 && S_ISDIR(st.st_mode));
    PASS();
}

/* P2: every request for a bound source's plain column is either guarded to
 * a ready source or a declared random-access site.  The definition and the
 * declaration (colrange.c / colrange.h) are skipped.  A `random:` marker
 * names one of the audited classes (Task 15's table), so a new site cannot
 * pass under an invented or misspelled reason. */
static const char* const plain_classes[] = {
    "index probe", "sort", "top-k sort key", "top-k gather (P3)", "selection gather (P3)",
    "holistic aggregate", "wide key", "indexed v2 strategy", "first-n", "slice group",
    "STR strlen", "join", "datalog", "pivot", "window", "user value",
    "unstage: scalar holistic or wide input", "unstage: v2 whole-column strategy",
    NULL,
};
static bool plain_marker_ok(const char* line) {
    if (strstr(line, "/* ready-only */")) return true;
    const char* m = strstr(line, "/* random: ");
    if (!m) return false;
    m += strlen("/* random: ");
    const char* end = strstr(m, " */");
    if (!end) return false;
    for (int i = 0; plain_classes[i]; i++) {
        size_t n = strlen(plain_classes[i]);
        if ((size_t)(end - m) == n && strncmp(m, plain_classes[i], n) == 0) return true;
    }
    return false;
}

static void check_plain_requests(const char* p, const char* rel, srccheck_acc_t* acc) {
    acc->nseen++;
    if (strcmp(rel, "src/table/table.c") == 0) acc->saw_table = true;
    if (strcmp(rel, "src/store/colrange.c") == 0 || strcmp(rel, "src/store/colrange.h") == 0) return;
    FILE* f = fopen(p, "rb");
    if (!f) return;
    char line[4096]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (!strstr(line, "ray_col_src_plain(")) continue;
        if (plain_marker_ok(line)) continue;
        if (!acc->nbad) snprintf(acc->bad, sizeof acc->bad, "%s:%d unmarked ray_col_src_plain", rel, ln);
        acc->nbad++;
    }
    fclose(f);
}

static test_result_t test_plain_requests_marked(void) {
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char dir[64]; snprintf(dir, sizeof dir, "%s/src", root);
    srccheck_acc_t acc = { "", 0, 0, false };
    walk(dir, "src", &acc, check_plain_requests);
    TEST_ASSERT_FMT(acc.saw_table && acc.nseen > 50, "source tree not scanned (%d files)", acc.nseen);
    TEST_ASSERT_FMT(acc.nbad == 0, "%s (%d sites)", acc.bad, acc.nbad);
    PASS();
}

/* The marker check itself: the accepted forms, and the near misses. */
static test_result_t test_plain_marker_forms(void) {
    TEST_ASSERT_TRUE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* ready-only */"));
    TEST_ASSERT_TRUE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: wide key */"));
    TEST_ASSERT_TRUE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: top-k gather (P3) */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: wide keys */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: a route that reads whole columns */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* ready: never decodes */"));
    PASS();
}

/* P1's transitional whole-table decode (carry 4) stays removed. */
static void check_no_table_decode(const char* p, const char* rel, srccheck_acc_t* acc) {
    static const char* const needles[] = { "TABLE_DECODE_OR_RETURN", NULL };
    acc->nseen++;
    if (strcmp(rel, "src/table/table.c") == 0) acc->saw_table = true;
    int ln = first_hit(p, needles);
    if (ln) {
        if (!acc->nbad) snprintf(acc->bad, sizeof acc->bad, "%s:%d names TABLE_DECODE_OR_RETURN", rel, ln);
        acc->nbad++;
    }
}
static test_result_t test_no_table_decode_or_return(void) {
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char dir[64]; snprintf(dir, sizeof dir, "%s/src", root);
    srccheck_acc_t acc = { "", 0, 0, false };
    walk(dir, "src", &acc, check_no_table_decode);
    TEST_ASSERT_FMT(acc.saw_table && acc.nseen > 50, "source tree not scanned (%d files)", acc.nseen);
    TEST_ASSERT_FMT(acc.nbad == 0, "%s (%d files)", acc.bad, acc.nbad);
    PASS();
}

const test_entry_t srccheck_entries[] = {
    { "srccheck/no_legacy_getters", test_no_legacy_getters, NULL, NULL },
    { "srccheck/no_raw_column_list",  test_no_raw_column_list,  NULL, NULL },
    { "srccheck/materializer_attach", test_materializer_attach, NULL, NULL },
    { "srccheck/cols_inplace_plain", test_cols_inplace_plain, NULL, NULL },
    { "srccheck/tmp_paths_per_process", test_tmp_paths_per_process, NULL, NULL },
    { "srccheck/plain_requests_marked",      test_plain_requests_marked,      NULL, NULL },
    { "srccheck/plain_marker_forms",         test_plain_marker_forms,         NULL, NULL },
    { "srccheck/no_table_decode_or_return",  test_no_table_decode_or_return,  NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
