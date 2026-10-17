/* P6 gate (plan: compressed-columns P6, Task 8): the runtime DICT of every
 * all-STR_DICT STR column of a generation-2 store against generation 0's
 * persisted DICT, compared as data (codes, first occurrences, n_distinct)
 * along with every element.
 *
 * Build: make lib && cc -O2 -std=c17 -Wall -Wextra -Iinclude -Isrc \
 *          bench/compress_estimate/p6_dictcheck.c librayforce.a -lm -lpthread \
 *          -o p6_dictcheck
 * Run:   p6_dictcheck <g0 dir> <g2 dir>
 * Prints one "dict" line per STR column:
 *   dict <column> <all STR_DICT> <gen-2 index> <DICT equal> <elements equal>
 *        <vectors> <STR_DICT vectors> <n_distinct gen 0> <n_distinct gen 2>
 * and a final "dictgate" line: PASS, FAIL, or "not exercised" (no
 * all-STR_DICT column: FAIL).  "All STR_DICT" is read from the descriptors
 * (no decode); it is the condition of the runtime DICT (every chunk then
 * carries code bounds).  A gen-2 DICT on a column that is not all STR_DICT
 * would be outside that condition and fails the gate. */
#include <rayforce.h>
#include "core/runtime.h"
#include "lang/eval.h"
#include "table/table.h"
#include "store/enccol.h"
#include "ops/idxop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ray_t* load(const char* dir) {
    char q[4200];
    snprintf(q, sizeof q, "(.db.splayed.get \"%s/\")", dir);
    ray_t* t = ray_eval_str(q);
    if (!t || RAY_IS_ERR(t)) { fprintf(stderr, "load %s: %s\n", dir, ray_error_msg()); exit(2); }
    return t;
}
/* Every vector of the column is STR_DICT (read from the descriptors; no
 * decode); *nv / *nsd count the vectors and the STR_DICT ones. */
static int all_strdict(ray_t* enc, int64_t* nvec, int64_t* nsd) {
    ray_enccol_t* e = ray_enccol_of(enc);
    int64_t nv = ray_enc_nvecs(e->len);
    int all = 1;
    *nvec = nv; *nsd = 0;
    for (int64_t c = 0; c < e->n_chunks; c++) {
        ray_enc_desc_t d[64];
        ray_t* err = ray_enccol_chunk_descs(e, c, d);
        if (err) { fprintf(stderr, "descs: %s\n", ray_err_code(err)); ray_error_free(err); return -1; }
        int64_t k = nv - c * 64 < 64 ? nv - c * 64 : 64;
        for (int64_t j = 0; j < k; j++) {
            if (d[j].encoding == RAY_ENC_STR_DICT) (*nsd)++;
            else all = 0;
        }
    }
    return all;
}
static int same_elems(ray_t* a, ray_t* b) {
    if (a->len != b->len) return 0;
    for (int64_t i = 0; i < a->len; i++) {
        size_t la, lb;
        const char* x = ray_str_vec_get(a, i, &la);
        const char* y = ray_str_vec_get(b, i, &lb);
        if (la != lb || (la && memcmp(x, y, la) != 0)) return 0;
    }
    return 1;
}
static int dict_same(ray_t* a, ray_t* b) {
    ray_index_t* x = ray_index_payload(a->index);
    ray_index_t* y = ray_index_payload(b->index);
    return x->u.dict.n_distinct == y->u.dict.n_distinct &&
           x->u.dict.codes->len == y->u.dict.codes->len &&
           x->u.dict.first_occ->len == y->u.dict.first_occ->len &&
           memcmp(ray_data(x->u.dict.codes), ray_data(y->u.dict.codes), (size_t)x->u.dict.codes->len * 4) == 0 &&
           memcmp(ray_data(x->u.dict.first_occ), ray_data(y->u.dict.first_occ), (size_t)x->u.dict.n_distinct * 4) == 0;
}
int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: p6_dictcheck <g0 dir> <g2 dir>\n"); return 2; }
    ray_runtime_t* rt = ray_runtime_create(0, NULL);
    ray_t* t0 = load(argv[1]);
    ray_t* t2 = load(argv[2]);
    int exercised = 0, bad = 0;
    for (int64_t i = 0; i < ray_table_ncols(t0); i++) {
        int64_t name = ray_table_col_name(t0, i);
        ray_col_meta_t m;
        if (!ray_table_col_meta(t2, name, &m) || m.type != RAY_STR) continue;
        ray_t* nm = ray_sym_str(name);
        int64_t nvec = 0, nsd = 0;
        int all = m.enc ? all_strdict(m.enc, &nvec, &nsd) : 0;
        if (all < 0) { bad = 1; printf("dict\t%.*s\terror\n", (int)ray_str_len(nm), ray_str_ptr(nm)); continue; }
        ray_t* a = ray_table_col(t0, name);
        ray_t* b = ray_table_col(t2, name);
        if (!a || RAY_IS_ERR(a) || !b || RAY_IS_ERR(b)) {
            fprintf(stderr, "%.*s: column: %s\n", (int)ray_str_len(nm), ray_str_ptr(nm), ray_error_msg());
            return 2;
        }
        int elems = same_elems(a, b);
        ray_idx_kind_t k2 = ray_index_kind(b);
        const char* kind2 = k2 == RAY_IDX_NONE ? "none" : k2 == RAY_IDX_DICT ? "dict" : k2 == RAY_IDX_HASH ? "hash" : "other";
        int g0dict = ray_index_kind(a) == RAY_IDX_DICT;
        int dict_eq = g0dict && k2 == RAY_IDX_DICT && dict_same(a, b);
        long long nd0 = g0dict ? (long long)ray_index_payload(a->index)->u.dict.n_distinct : -1;
        long long nd2 = k2 == RAY_IDX_DICT ? (long long)ray_index_payload(b->index)->u.dict.n_distinct : -1;
        const char* eq = all || k2 == RAY_IDX_DICT ? (dict_eq ? "true" : "false") : "-";
        printf("dict\t%.*s\t%s\t%s\t%s\t%s\t%lld\t%lld\t%lld\t%lld\n", (int)ray_str_len(nm), ray_str_ptr(nm),
               all ? "true" : "false", kind2, eq, elems ? "true" : "false",
               (long long)nvec, (long long)nsd, nd0, nd2);
        if (all) { exercised++; if (!dict_eq) bad = 1; }
        else if (k2 == RAY_IDX_DICT) bad = 1;     /* a DICT outside its condition */
        if (!elems) bad = 1;
    }
    printf("dictgate\t%s\t%d\n", !exercised ? "not exercised" : bad ? "FAIL" : "PASS", exercised);
    ray_release(t0); ray_release(t2);
    ray_runtime_destroy(rt);
    return 0;
}
