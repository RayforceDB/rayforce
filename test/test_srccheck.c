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
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p ||
            snprintf(r, sizeof r, "%s/%s", rel, e->d_name) >= (int)sizeof r) {   /* never skip a file unseen */
            if (!acc->nbad) snprintf(acc->bad, sizeof acc->bad, "a path under %.400s is too long to check", rel);
            acc->nbad++;
            continue;
        }
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
        if (!acc->nbad && snprintf(acc->bad, sizeof acc->bad, "%s:%d uses a legacy getter", rel, ln) < 0) acc->bad[0] = 0;   /* a long message is cut */
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
    snprintf(path, sizeof path, "%s/src/store/encidx.c", root);    /* the index-region parse */
    TEST_ASSERT_TRUE(stat(path, &st) == 0);
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
    char root[4096];
    const char* base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    snprintf(root, sizeof root, "%s/rayforce_t.%ld", base, (long)getpid());
    char want[12544];
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
    "index probe", "sort", "top-k sort key",
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

/* One source line requests a plain column without a marker on that line. */
static bool plain_line_bad(const char* line) {
    return strstr(line, "ray_col_src_plain(") && !plain_marker_ok(line);
}

/* Unmarked request lines in `text`, as the tree check counts them in a
 * file. */
static int plain_text_bad(const char* text) {
    char line[4096]; int bad = 0;
    while (*text) {
        const char* nl = strchr(text, '\n');
        size_t n = nl ? (size_t)(nl - text) : strlen(text);
        if (n >= sizeof line) n = sizeof line - 1;
        memcpy(line, text, n); line[n] = 0;
        if (plain_line_bad(line)) bad++;
        text += nl ? n + 1 : n;
    }
    return bad;
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
        if (!plain_line_bad(line)) continue;
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
    TEST_ASSERT_TRUE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: top-k sort key */"));
    /* the P3 gather classes are gone: those sites gather by range now */
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: top-k gather (P3) */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: selection gather (P3) */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: wide keys */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* random: a route that reads whole columns */"));
    TEST_ASSERT_FALSE(plain_marker_ok("x = ray_col_src_plain(s, &c);   /* ready: never decodes */"));
    /* The check is a text match on each line, not a parse: */
    /* a marker on the following line does not mark the call; */
    TEST_ASSERT_EQ_I(plain_text_bad("x = ray_col_src_plain(s, &c);\n/* random: wide key */\n"), 1);
    /* a call written in a // comment counts as a call; */
    TEST_ASSERT_EQ_I(plain_text_bad("// ray_col_src_plain(s, &c) reads the column whole\n"), 1);
    /* one marker covers every call on its line. */
    TEST_ASSERT_EQ_I(plain_text_bad("a = ray_col_src_plain(s, &c); b = ray_col_src_plain(t, &d);   /* ready-only */\n"), 0);
    TEST_ASSERT_EQ_I(plain_text_bad("a = ray_col_src_plain(s, &c); b = ray_col_src_plain(t, &d);\n"), 1);
    PASS();
}

/* P3 Task 6b: the first-n pre-pass (agg_first_n_run) reads its keys at
 * rows through their sources and gathers its groups' rows by range: its
 * body neither unstages a key nor requests a plain column.  The other
 * whole-column strategies (indexed, serial) keep their marked sites. */
static test_result_t test_firstn_reads_by_range(void) {
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char path[128]; snprintf(path, sizeof path, "%s/src/ops/agg_engine.c", root);
    FILE* f = fopen(path, "rb");
    TEST_ASSERT_TRUE(f != NULL);
    char line[4096]; int ln = 0, body = 0, bad = 0, lines = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (!body && strncmp(line, "static ray_t* agg_first_n_run(", 30) == 0 && !strchr(line, ';')) body = ln;
        if (!body) continue;
        lines++;
        if (strstr(line, "agg_keys_unstage(") || strstr(line, "ray_col_src_plain(")) { if (!bad) bad = ln; }
        if (line[0] == '}') break;
    }
    fclose(f);
    TEST_ASSERT_FMT(body && lines > 50, "agg_first_n_run not found (%d lines)", lines);
    TEST_ASSERT_FMT(bad == 0, "src/ops/agg_engine.c:%d: agg_first_n_run reads a key whole", bad);
    PASS();
}

/* P1's transitional whole-table decode (carry 4) stays removed. */
static void check_no_table_decode(const char* p, const char* rel, srccheck_acc_t* acc) {
    static const char* const needles[] = { "TABLE_DECODE_OR_RETURN", NULL };
    acc->nseen++;
    if (strcmp(rel, "src/table/table.c") == 0) acc->saw_table = true;
    int ln = first_hit(p, needles);
    if (ln) {
        if (!acc->nbad && snprintf(acc->bad, sizeof acc->bad, "%s:%d names TABLE_DECODE_OR_RETURN", rel, ln) < 0) acc->bad[0] = 0;   /* a long message is cut */
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

/* P3's gate (Task 12): the P3 random-access classes are gone, every one of
 * those sites gathers by range, and none comes back: no audited class names
 * a P3 deferral, and no `random:` marker in the source carries "(P3)". */
static bool p3_marker_line(const char* line) {
    for (const char* m = strstr(line, "random:"); m; m = strstr(m + 1, "random:")) {
        const char* end = strstr(m, "*/");
        const char* p3 = strstr(m, "(P3)");
        if (p3 && (!end || p3 < end)) return true;
    }
    return false;
}
static void check_no_p3_markers(const char* p, const char* rel, srccheck_acc_t* acc) {
    acc->nseen++;
    if (strcmp(rel, "src/table/table.c") == 0) acc->saw_table = true;
    FILE* f = fopen(p, "rb");
    if (!f) return;
    char line[4096]; int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        if (!p3_marker_line(line)) continue;
        if (!acc->nbad) snprintf(acc->bad, sizeof acc->bad, "%s:%d a (P3) random-access marker", rel, ln);
        acc->nbad++;
    }
    fclose(f);
}
static test_result_t test_no_p3_markers(void) {
    for (int i = 0; plain_classes[i]; i++)
        TEST_ASSERT_FMT(!strstr(plain_classes[i], "(P3)"), "plain_classes names \"%s\"", plain_classes[i]);
    /* the line check itself */
    TEST_ASSERT_TRUE(p3_marker_line("x = ray_col_src_plain(s, &c);   /* random: top-k gather (P3) */"));
    TEST_ASSERT_TRUE(p3_marker_line("/* ready-only */ y(); /* random: selection gather (P3) */"));
    TEST_ASSERT_TRUE(p3_marker_line(" * random: a wide gather (P3), until the views land"));
    TEST_ASSERT_FALSE(p3_marker_line("x = ray_col_src_plain(s, &c);   /* random: wide key */"));
    TEST_ASSERT_FALSE(p3_marker_line("/* random: sort */ /* gathers by range since (P3) Task 6 */"));
    TEST_ASSERT_FALSE(p3_marker_line("/* gathers by range since (P3) Task 6 */"));
    struct stat st;
    const char* root = stat("src/table/table.c", &st) == 0 ? "." : "..";
    char dir[64]; snprintf(dir, sizeof dir, "%s/src", root);
    srccheck_acc_t acc = { "", 0, 0, false };
    walk(dir, "src", &acc, check_no_p3_markers);
    TEST_ASSERT_FMT(acc.saw_table && acc.nseen > 50, "source tree not scanned (%d files)", acc.nseen);
    TEST_ASSERT_FMT(acc.nbad == 0, "%s (%d lines)", acc.bad, acc.nbad);
    PASS();
}

/* `text` with comments and string / character literals blanked to spaces
 * (newlines kept, so line numbers hold).  Caller frees. */
static char* code_only(const char* text) {
    size_t n = strlen(text);
    char* o = malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, text, n + 1);
    for (size_t i = 0; i < n; ) {
        if (text[i] == '/' && text[i + 1] == '*') {
            size_t j = i + 2;
            while (j < n && !(text[j] == '*' && text[j + 1] == '/')) j++;
            j = j < n ? j + 2 : n;
            for (size_t k = i; k < j; k++) if (o[k] != '\n') o[k] = ' ';
            i = j;
        } else if (text[i] == '/' && text[i + 1] == '/') {
            while (i < n && text[i] != '\n') o[i++] = ' ';
        } else if (text[i] == '"' || text[i] == '\'') {
            char q = text[i];
            size_t j = i + 1;
            while (j < n && text[j] != q && text[j] != '\n') j += text[j] == '\\' && j + 1 < n ? 2 : 1;
            j = j < n && text[j] == q ? j + 1 : j;
            for (size_t k = i; k < j; k++) if (o[k] != '\n') o[k] = ' ';
            i = j;
        } else {
            i++;
        }
    }
    return o;
}

static bool src_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/* A floating identifier: a token equal to a floating type or macro, a
 * token containing a classifier or magnitude / sign helper (so
 * __builtin_signbit, isnanf, fabsl, _Float64x match too), or a compiler
 * builtin naming an infinity or NaN (__builtin_inf, __builtin_nanf). */
static bool float_ident(const char* t, size_t n) {
    static const char* const exact[] = { "double", "float", "NAN", "INFINITY", NULL };
    static const char* const inside[] = { "signbit", "isnan", "isinf", "isfinite", "isnormal", "fpclassify",
                                          "fabs", "copysign", "HUGE_VAL", "_Float", NULL };
    static const char* const builtin[] = { "inf", "nan", "huge_val", NULL };
    char tok[256];
    if (n >= sizeof tok) n = sizeof tok - 1;
    memcpy(tok, t, n); tok[n] = 0;
    for (int k = 0; exact[k]; k++) if (strcmp(tok, exact[k]) == 0) return true;
    for (int k = 0; inside[k]; k++) if (strstr(tok, inside[k])) return true;
    if (strncmp(tok, "__builtin_", 10) == 0)
        for (int k = 0; builtin[k]; k++) if (strncmp(tok + 10, builtin[k], strlen(builtin[k])) == 0) return true;
    return false;
}

/* Line of the first floating construct in the code of `text` (comments and
 * literals skipped), 0 if none: a floating identifier (float_ident), or a
 * floating literal (a decimal number with '.' or an exponent, a hex number
 * with '.' or a 'p' exponent). */
static int float_code_hit(const char* text) {
    char* c = code_only(text);
    if (!c) return -1;
    int line = 1, hit = 0;
    for (size_t i = 0; c[i] && !hit; ) {
        if (c[i] == '\n') { line++; i++; continue; }
        bool num = (c[i] >= '0' && c[i] <= '9') || (c[i] == '.' && c[i + 1] >= '0' && c[i + 1] <= '9');
        if (src_ident_char(c[i]) && !num) {
            size_t j = i;
            while (src_ident_char(c[j])) j++;
            if (float_ident(c + i, j - i)) hit = line;
            i = j;
        } else if (num) {
            bool hex = c[i] == '0' && (c[i + 1] == 'x' || c[i + 1] == 'X');
            bool fl = false;
            size_t j = i;
            while (src_ident_char(c[j]) || c[j] == '.') {
                char ch = c[j];
                if (ch == '.') fl = true;
                if (hex ? (ch == 'p' || ch == 'P') : (j > i && (ch == 'e' || ch == 'E'))) {
                    fl = true;
                    if (c[j + 1] == '+' || c[j + 1] == '-') j++;
                }
                j++;
            }
            if (fl) hit = line;
            i = j;
        } else {
            i++;
        }
    }
    free(c);
    return hit;
}

static char* src_slurp(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char* t = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (t) { size_t r = fread(t, 1, (size_t)n, f); t[r] = 0; }
    fclose(f);
    return t;
}

/* The index-region validator, writer and parse decide float rules on bits:
 * release builds use -fno-signed-zeros, under which a zero's sign test
 * or a compare against zero may fold away (P4 Task 3 review).  The check
 * reads code only: a comment or message may say "10.0" or "doubles". */
static test_result_t test_encidx_float_bits(void) {
    /* the matcher itself */
    TEST_ASSERT_EQ_I(float_code_hit("/* 10.0 doubles, signbit( */ int x;"), 0);
    TEST_ASSERT_EQ_I(float_code_hit("int a; // 0.0 double isnan\nint b;"), 0);
    TEST_ASSERT_EQ_I(float_code_hit("f(\"0.0 double float\", 'x', '\\'');"), 0);
    TEST_ASSERT_EQ_I(float_code_hit("int doubles = 10, my_double = 0x1Eu, d2 = 07;"), 0);
    TEST_ASSERT_EQ_I(float_code_hit("if (signbit(x)) return;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("int a;\n  double y;"), 2);
    TEST_ASSERT_EQ_I(float_code_hit("/* x */\n/* y */ b = a < 0.0;"), 2);
    TEST_ASSERT_EQ_I(float_code_hit("b = a < 10.0;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("x = 1e3;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("x = .5f;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("x = 0x1p3;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("char q = '\"'; float z;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("s = \"a\\\"b\"; isnan(z);"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("/* __builtin_signbit( __builtin_isnan( */ int x;"), 0);
    TEST_ASSERT_EQ_I(float_code_hit("// __builtin_inf() __builtin_nan(\"\")\nint x;"), 0);
    TEST_ASSERT_EQ_I(float_code_hit("if (__builtin_signbit(x)) return;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("int a;\nb = __builtin_isnan(x);"), 2);
    TEST_ASSERT_EQ_I(float_code_hit("x = __builtin_inf();"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("x = __builtin_nan(\"\");"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("y = fabs(x) + copysign(a, b);"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("y = HUGE_VAL; _Float64 z;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("y = NAN;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("y = INFINITY;"), 1);
    TEST_ASSERT_EQ_I(float_code_hit("int nan_count = __builtin_popcountll(x), infos;"), 0);
    struct stat st;
    const char* root = stat("src/store/encidx.c", &st) == 0 ? "." : "..";
    char p[128]; snprintf(p, sizeof p, "%s/src/store/encidx.c", root);
    char* text = src_slurp(p);
    TEST_ASSERT_NOT_NULL(text);
    int ln = float_code_hit(text);
    free(text);
    TEST_ASSERT_FMT(ln == 0, "src/store/encidx.c:%d compares floating values", ln);
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
    { "srccheck/firstn_reads_by_range",      test_firstn_reads_by_range,      NULL, NULL },
    { "srccheck/no_p3_markers",              test_no_p3_markers,              NULL, NULL },
    { "srccheck/encidx_float_bits",          test_encidx_float_bits,          NULL, NULL },
    { NULL, NULL, NULL, NULL },
};
