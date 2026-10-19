/* P6b gate (plan: compressed-columns P6b, Task 8): the persisted DICT of
 * every STR column generation 0 indexes, against generation 0's DICT,
 * compared as data (codes, first occurrences, n_distinct) along with every
 * element.  Generation 2 is checked twice: before materialization, the
 * region through ray_enccol_idict and the code tier
 * (ray_enccol_idict_codes_vec); after it, the index attached to the
 * materialized root.
 *
 * Build: make lib && cc -O2 -std=c17 -Wall -Wextra -Iinclude -Isrc \
 *          bench/compress_estimate/p6_dictcheck.c librayforce.a -lm -lpthread \
 *          -o p6_dictcheck
 * Run:   p6_dictcheck <g0 dir> <g2 dir>
 * Prints one "dict" line per STR column:
 *   dict <column> <g0 DICT> <g2 kind> <region equal> <root equal>
 *        <elements equal> <vectors> <STR_DICT vectors> <n_distinct gen 0>
 *        <n_distinct gen 2>
 * and a final "dictgate" line: PASS, FAIL, or "not exercised" (no STR column
 * with a generation-0 DICT: FAIL).  <g2 kind> is the kind generation 2
 * records ("dict" or "none"); the equalities are "-" where generation 0 has
 * no DICT.  A column fails when generation 0 has a DICT and generation 2
 * records none, or either equality fails, or the elements differ, or
 * generation 2 has a DICT (recorded or attached) where generation 0 has none.
 * The vector counts (all_strdict, from the descriptors, no decode) are
 * informational. */
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
        int nl = (int)ray_str_len(nm); const char* np = ray_str_ptr(nm);
        int64_t nvec = 0, nsd = 0;
        int all = m.enc ? all_strdict(m.enc, &nvec, &nsd) : 0;
        if (all < 0) { bad = 1; printf("dict\t%.*s\terror\n", nl, np); continue; }
        ray_t* a = ray_table_col(t0, name);
        if (!a || RAY_IS_ERR(a)) { fprintf(stderr, "%.*s: column g0: %s\n", nl, np, ray_error_msg()); return 2; }
        /* generation 0 indexes it: its passenger DICT */
        int g0dict = ray_index_kind(a) == RAY_IDX_DICT;
        /* generation 2: the recorded kind, the region (before materialization), the root (after) */
        ray_enccol_t* e = m.enc ? ray_enccol_of(m.enc) : NULL;
        const ray_enc_idict_t* d = NULL;
        ray_t* derr = e ? ray_enccol_idict(e, &d) : NULL;
        ray_t* cv = NULL;
        if (!derr && d) derr = ray_enccol_idict_codes_vec(e, &cv);
        if (derr) { fprintf(stderr, "%.*s: %s\n", nl, np, ray_error_msg()); return 2; }
        ray_index_t* x = g0dict ? ray_index_payload(a->index) : NULL;
        int region_eq = x && d && d->n_distinct == x->u.dict.n_distinct &&
                        d->len == x->u.dict.codes->len && cv && cv->len == d->len &&
                        memcmp(ray_data(cv), ray_data(x->u.dict.codes), (size_t)d->len * 4) == 0 &&
                        memcmp(d->first_occ, ray_data(x->u.dict.first_occ), (size_t)d->n_distinct * 4) == 0;
        ray_t* b = ray_table_col(t2, name);            /* materialize */
        if (!b || RAY_IS_ERR(b)) { fprintf(stderr, "%.*s: column g2: %s\n", nl, np, ray_error_msg()); return 2; }
        ray_idx_kind_t k2 = ray_index_kind(b);
        int root_eq = x && k2 == RAY_IDX_DICT && dict_same(a, b);
        int elems = same_elems(a, b);
        long long nd0 = x ? (long long)x->u.dict.n_distinct : -1;
        long long nd2 = d ? (long long)d->n_distinct : k2 == RAY_IDX_DICT ? (long long)ray_index_payload(b->index)->u.dict.n_distinct : -1;
        /* line: dict <col> <g0 DICT> <g2 kind> <region equal> <root equal> <elements equal> <vectors> <STR_DICT vectors> <nd g0> <nd g2> */
        printf("dict\t%.*s\t%s\t%s\t%s\t%s\t%s\t%lld\t%lld\t%lld\t%lld\n", nl, np,
               g0dict ? "true" : "false", d ? "dict" : "none",
               g0dict ? (region_eq ? "true" : "false") : "-", g0dict ? (root_eq ? "true" : "false") : "-",
               elems ? "true" : "false", (long long)nvec, (long long)nsd, nd0, nd2);
        if (g0dict) { exercised++; if (!d || !region_eq || !root_eq) bad = 1; }
        else if (d || k2 == RAY_IDX_DICT) bad = 1;     /* a generation-2 DICT where generation 0 has none */
        if (!elems) bad = 1;
    }
    printf("dictgate\t%s\t%d\n", !exercised ? "not exercised" : bad ? "FAIL" : "PASS", exercised);
    ray_release(t0); ray_release(t2);
    ray_runtime_destroy(rt);
    return 0;
}
