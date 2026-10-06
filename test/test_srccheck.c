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

/* Files not yet migrated off the legacy getters.  Part B removes an entry in
 * the task that migrates the file; this list is the only place to edit. */
static const char* const srccheck_pending[] = {
    "src/ops/query.c",
    NULL,
};

static bool is_pending(const char* path) {
    for (int i = 0; srccheck_pending[i]; i++) if (strcmp(srccheck_pending[i], path) == 0) return true;
    return false;
}

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
    bool pend = is_pending(rel);
    if ((ln && !pend) || (!ln && pend)) {
        if (!acc->nbad) {
            if (ln) snprintf(acc->bad, sizeof acc->bad, "%s:%d uses a legacy getter", rel, ln);
            else snprintf(acc->bad, sizeof acc->bad,
                          "%s has no legacy getter left: remove it from srccheck_pending", rel);
        }
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

const test_entry_t srccheck_entries[] = {
    { "srccheck/no_legacy_getters", test_no_legacy_getters, NULL, NULL },
    { "srccheck/cols_inplace_plain", test_cols_inplace_plain, NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
