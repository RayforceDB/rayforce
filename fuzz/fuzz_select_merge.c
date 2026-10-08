/* Structured differential fuzzer for nested select composition (#752).
 * Compare two/three literal layers against do-wrapped materialization,
 * including rejected shapes. The oracle checks wire bytes or error codes.
 */
#include "common.h"
#include "lang/eval.h"
#include "store/serde.h"
#include "vec/vec.h"
#include <math.h>

static void drop(ray_t* x) {
    if (x && RAY_IS_ERR(x)) ray_error_free(x);
    else if (x) ray_release(x);
}

static unsigned byte(const uint8_t* data, size_t size, size_t* at) {
    return size ? data[(*at)++ % size] : 0;
}

/* Fusing a floating sum can change its addition order. Permit roundoff only
 * in the generated aggregate `total`; row expressions, schema and wire attrs
 * still compare exactly, including nulls created by extreme F64 arithmetic. */
static bool aggregate_roundoff(ray_t* a, ray_t* b) {
    if (a->type != RAY_TABLE || b->type != RAY_TABLE ||
        ray_table_ncols(a) != ray_table_ncols(b) || ray_table_nrows(a) != ray_table_nrows(b) ||
        (a->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SORTED)) !=
        (b->attrs & (RAY_ATTR_HAS_NULLS | RAY_ATTR_SORTED))) return false;
    int64_t total = ray_sym_intern("total", 5);
    for (int64_t i = 0; i < ray_table_ncols(a); i++) {
        int64_t name = ray_table_col_name(a, i);
        if (name != ray_table_col_name(b, i)) return false;
        ray_t* x = ray_table_get_col_idx(a, i), *y = ray_table_get_col_idx(b, i);
        ray_t* sx = ray_ser(x), *sy = ray_ser(y);
        bool same = sx && sy && !RAY_IS_ERR(sx) && !RAY_IS_ERR(sy) && sx->len == sy->len &&
                    !memcmp(ray_data(sx), ray_data(sy), sx->len);
        drop(sx); drop(sy);
        if (same) continue;
        if (name != total || x->type != RAY_F64 || y->type != RAY_F64 || x->len != y->len ||
            (x->attrs & RAY_ATTR_HAS_NULLS) != (y->attrs & RAY_ATTR_HAS_NULLS)) return false;
        const double* xv = ray_data(x), *yv = ray_data(y);
        for (int64_t row = 0; row < x->len; row++) {
            bool xn = ray_vec_is_null(x, row), yn = ray_vec_is_null(y, row);
            if (xn != yn) return false;
            if (xn) continue;
            if (!isfinite(xv[row]) || !isfinite(yv[row]) ||
                fabs(xv[row] - yv[row]) > 1e-12 * fmax(1.0, fmax(fabs(xv[row]), fabs(yv[row])))) return false;
        }
    }
    return true;
}

static void query(char* out, const unsigned* choices, bool barrier) {
    static const char* const filters[] = {
        "", "where:(== a 1)", "where:(> p 3)", "where:(== s 'beta)",
        "where:(== text \"b\")", "where:(and (== a 1) (== b 1))",
        "where:(== a 9)", "where:(== p 0Nf)"
    };
    static const char* const projections[] = {
        "", "a:a b:b k:k p:p s:s text:text", "a:a b:b k:k p:p s:s text:text q:(* p 2)",
        "p:p q:(- p (avg p))", "p:p q:(sums p)", "p:p q:(reverse p)",
        "p:p bad:missing", "p:p q:(missing p)", "take:2", "desc:p"
    };
    static const char* const outputs[] = {
        "", "p:p s:s text:text", "total:(sum p)", "by:k total:(sum p)",
        "total:(sum q)", "q:q", "take:3", "p:p desc:p take:2",
        "a:(+ a 1) r:a", "total:(count s)"
    };
    static const char* const computed[] = {"(* p 2)", "(* p 1.7)", "(+ p 1.3)", "(- p 0.7)"};
    char projection[256];
    snprintf(projection, sizeof(projection), "a:a b:b k:k p:p s:s text:text q:%s", computed[(choices[1] / 10) % 4]);
    snprintf(out, 4096, "(select {from:T %s %s})", filters[choices[0] % 8],
             choices[1] % 10 == 2 ? projection : projections[choices[1] % 10]);
    for (unsigned layer = 0; layer < 1 + choices[2] % 2; layer++) {
        char prev[4096];
        strcpy(prev, out);
        /* Each layer adds at most 200 bytes; the generated depth is <= 3. */
        snprintf(out, 4096, "(select {from:%s%.3000s%s %s %s})", barrier ? "(do " : "", prev,
                 barrier ? ")" : "", filters[choices[3 + layer * 2] % 8],
                 outputs[choices[4 + layer * 2] % 10]);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!ray_fuzz_ready && getenv("RAY_FUZZ_CORES")) {
        setenv("RAYFORCE_CORES", getenv("RAY_FUZZ_CORES"), 1);
        ray_runtime_create(0, NULL);
        ray_fuzz_ready = 1;
    } else ray_fuzz_init();
    size_t at = 0;
    unsigned choices[7];
    for (int i = 0; i < 7; i++) choices[i] = byte(data, size, &at);
    unsigned rows = byte(data, size, &at) % 33;
    char src[8192]; size_t pos = 0;
    pos += snprintf(src + pos, sizeof(src) - pos, "(set T (table [a b k p s text] (list ");
    for (int col = 0; col < 6; col++) {
        if (!rows) {
            static const char* const empty[] = {"[0]", "[0]", "[0]", "[0.0]", "[alpha]", "[\"a\"]"};
            pos += snprintf(src + pos, sizeof(src) - pos, "(take %s 0) ", empty[col]);
            continue;
        }
        src[pos++] = '[';
        for (unsigned row = 0; row < rows; row++) {
            unsigned v = byte(data, size, &at);
            if (col == 4) {
                const char* sym[] = {"'", "alpha", "beta"};
                pos += snprintf(src + pos, sizeof(src) - pos, "%s ", sym[v % 3]);
            } else if (col == 5) {
                const char* str[] = {"\"\"", "\"a\"", "\"b\""};
                pos += snprintf(src + pos, sizeof(src) - pos, "%s ", str[v % 3]);
            } else if (v % 7 == 0 && !(col == 3 && v >= 250 && v <= 253)) {
                pos += snprintf(src + pos, sizeof(src) - pos, "%s ", col == 3 ? "0Nf" : "0N");
            } else if (col == 3) {
                const char* edge = v == 250 ? "1.7976931348623157e308" :
                                   v == 251 ? "-1.7976931348623157e308" :
                                   v == 252 ? "1e300" : v == 253 ? "-1e300" : NULL;
                if (edge) pos += snprintf(src + pos, sizeof(src) - pos, "%s ", edge);
                else pos += snprintf(src + pos, sizeof(src) - pos, "%u.%u ", v % 17, (v / 17) % 10);
            } else {
                pos += snprintf(src + pos, sizeof(src) - pos, "%u ", v % 3);
            }
        }
        src[pos++] = ']'; src[pos++] = ' ';
    }
    snprintf(src + pos, sizeof(src) - pos, ")))" );
    ray_t* t = ray_eval_str(src);
    if (!t || RAY_IS_ERR(t)) abort();
    drop(t);
    /* Occasionally cross the merge threshold, morsel boundaries and the
     * parallel dispatch threshold. A short input still controls all values. */
    unsigned scale = choices[2] >> 4;
    int large_rows = scale == 12 ? 8191 : scale == 13 ? 8192 :
                     scale == 14 ? 16385 : scale == 15 ? 131073 : 0;
    if (rows && large_rows) {
        char grow[64];
        snprintf(grow, sizeof(grow), "(set T (take T %d))", large_rows);
        t = ray_eval_str(grow);
        if (!t || RAY_IS_ERR(t)) abort();
        drop(t);
    }
    char optimized[4096], reference[4096];
    query(optimized, choices, false); query(reference, choices, true);
    ray_t* got = ray_eval_str(optimized), *want = ray_eval_str(reference);
    bool equal = got && want && RAY_IS_ERR(got) == RAY_IS_ERR(want);
    if (equal && RAY_IS_ERR(got)) {
        equal = got->slen == want->slen && !memcmp(got->sdata, want->sdata, got->slen);
    } else if (equal) {
        ray_t* a = ray_ser(got), *b = ray_ser(want);
        equal = a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b) &&
                a->len == b->len && !memcmp(ray_data(a), ray_data(b), a->len);
        if (!equal) equal = aggregate_roundoff(got, want);
        if (!equal && a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b)) {
            fprintf(stderr, "Serialized lengths: %lld / %lld\n", (long long)a->len, (long long)b->len);
            for (int64_t i = 0; i < a->len && i < b->len; i++)
                if (((uint8_t*)ray_data(a))[i] != ((uint8_t*)ray_data(b))[i])
                    fprintf(stderr, "Byte %lld: %u / %u\n", (long long)i, ((uint8_t*)ray_data(a))[i], ((uint8_t*)ray_data(b))[i]);
        }
        drop(a); drop(b);
    }
    if (!equal) {
        fprintf(stderr, "Fixture: %s\nExpanded rows: %d\nOptimized: %s\nReference: %s\n", src, large_rows, optimized, reference);
        abort();
    }
    drop(got); drop(want);
    return 0;
}
