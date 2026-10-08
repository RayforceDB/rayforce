/* Nested select composition: admission and serialized differential checks. */
#include "test.h"
#include "lang/eval.h"
#include "lang/env.h"
#include "ops/internal.h"
#include "store/serde.h"
#include "core/runtime.h"

extern ray_runtime_t* __RUNTIME;
static void setup(void) { ray_runtime_create(0, NULL); }
static void teardown(void) { ray_runtime_destroy(__RUNTIME); }

static void drop(ray_t* x) {
    if (x && RAY_IS_ERR(x)) ray_error_free(x);
    else if (x) ray_release(x);
}

/* `do` preserves evaluation but hides each literal nested select from the
 * rewrite. The reference exercises the old materializing execution path. */
static void materialized(const char* src, char* dst) {
    while (*src) {
        if (!strncmp(src, "from: (select", 13)) {
            memcpy(dst, "from: (do ", 10); dst += 10; src += 6;
            const char* start = src;
            int depth = 0;
            do { if (*src == '(') depth++; if (*src == ')') depth--; src++; } while (*src && depth);
            size_t len = (size_t)(src - start);
            char inner[4096];
            memcpy(inner, start, len); inner[len] = 0;
            materialized(inner, dst); dst += strlen(dst);
            *dst++ = ')';
        } else *dst++ = *src++;
    }
    *dst = 0;
}

static test_result_t test_select_merge_cases(void) {
    ray_t* t = ray_eval_str("(set T (table [a b k p s text] (list [0 1 1 0 1] [1 1 0 1 1] [0 1 0 1 0] [1.0 2.0 0Nf 4.0 5.0] [' alpha beta alpha beta] [\"\" \"a\" \"b\" \"a\" \"b\"])))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "fixture"); drop(t);
    const struct { const char* query; bool merge; } cases[] = {
        {"{from: (select {from: T where: (== a 1)}) where: (== b 1) total: (sum p)}", true},
        {"{from: (select {from: T where: (== a 1) a:a b:b k:k p:p s:s text:text q:(* p 2)}) by:k total:(sum q)}", true},
        {"{from: (select {from: T where: (== a 1) p:p k:k}) take: 2}", true},
        {"{from: (select {from: T a:a b:b k:k p:p s:s text:text}) where:(== a 1) total:(sum p)}", true},
        {"{from: (select {from: T where:(== s 'beta)}) where:(== text \"b\") p:p s:s text:text}", true},
        {"{from: (select {from: T where:(== a 9)}) p:p s:s text:text}", true},
        {"{from: (select {from: T where:(== a 9) a:a p:p}) where:(> p 3) total:(sum p)}", false},
        {"{from: (select {from: T k:k q:(* p 2) r:(* q 3)}) by:k total:(sum r) asc:k}", true},
        {"{from: (select {from: T p:p q:(- p 0.7)}) where:(== p 0Nf) total:(sum q)}", false},
        {"{from: (select {from: T q:(+ a 1)}) total:(sum q)}", false},
        {"{from: (select {from: T z:p k:k}) total:(sum z)}", true},
        {"{from: (select {from: T z:p k:k}) total:(sum 'z)}", true},
        {"{from: (select {from: T k:k q:(* p 2)}) total:(sum q) desc:total take:1}", true},
        {"{from: (select {from: T k:k q:(* p 2)}) where:(> q 3)}", true},
        {"{from: (select {from: (select {from: T where:(== a 1)}) where:(== b 1)}) total:(sum p)}", true},
        {"{from: (select {from: T where:(== a 1)}) p:p desc:p take:2}", true},
        /* Aggregates/windows/length changes cannot move across an outer filter. */
        {"{from: (select {from: T p:p q:(- p (avg p))}) where:(> p 1) q:q}", false},
        {"{from: (select {from: T p:p q:(sums p)}) where:(> p 1) q:q}", false},
        {"{from: (select {from: T p:p q:(reverse p)}) where:(> p 1) q:q}", false},
        {"{from: (select {from: T p:p q:42}) where:(> p 1) q:q}", false},
        {"{from: (select {from: T p:p q:(/ p 0)}) where:(> p 1) q:q}", false},
        /* Preserve missing names, dropped outputs, lexical scope and errors. */
        {"{from: (select {from: T p:p}) a:a}", false},
        {"{from: (select {from: T p:p missing:missing}) p:p}", false},
        {"{from: (select {from: T p:p bad:(as 'I64 text)}) p:p}", false},
        {"{from: (select {from: T p:p q:(* p 2)}) p:p}", false},
        {"{from: (select {from: T p:p}) s:'a}", false},
        {"{from: (select {from: T p:p}) asc:a}", false},
        {"{from: (select {from: T p:p}) q:(sum a)}", false},
        {"{from: (select {from: T a:(+ a 1) p:p}) where:(== a 1) p:p}", false},
        {"{from: (select {from: T q:(* p 2)}) p:(+ q 1) r:q}", false},
        {"{from: (select {from: T p:p}) q:(+ p 1) r:(+ q 1)}", false},
        {"{from: (select {from: T take:2}) p:p}", false},
        {"{from: (select {from: T desc:p}) p:p}", false},
        {"{from: (select {from: T by:k total:(sum p)}) total:total}", false},
        {"{from: (select {from: T where:(== a 1)}) where:(== b 1) take:2}", false},
        {"{from: (select {from: (do T) where:(== a 1)}) p:p}", false},
        {"{from: (select {from: T p:p}) by:a total:(sum p)}", false},
        {"{from: (select {from: T p:p}) where:(> a 1) p:p}", false},
        {"{from: (select {from: T p:p s:s text:text q:(* p 2)}) where:(== text \"b\") take:3}", false},
        {"{from: (select {from: T where:(> p 3)}) take:3}", false},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        char src[4096], ref[8192];
        snprintf(src, sizeof(src), "(quote %s)", cases[i].query);
        ray_t* dict = ray_eval_str(src);
        TEST_ASSERT(dict && !RAY_IS_ERR(dict), "query AST");
        ray_t* plan = ray_select_merge_plan(dict);
        TEST_ASSERT_FMT((plan != NULL) == cases[i].merge, "admission case %zu: %s", i, cases[i].query);
        drop(plan); drop(dict);
        snprintf(src, sizeof(src), "(select %s)", cases[i].query);
        materialized(src, ref);
        ray_t* got = ray_eval_str(src);
        char got_error[512] = {0};
        if (RAY_IS_ERR(got) && ray_error_msg()) snprintf(got_error, sizeof(got_error), "%s", ray_error_msg());
        ray_t* want = ray_eval_str(ref);
        TEST_ASSERT_FMT(got && want && RAY_IS_ERR(got) == RAY_IS_ERR(want), "error parity case %zu", i);
        if (RAY_IS_ERR(got)) {
            TEST_ASSERT_FMT(got->slen == want->slen && !memcmp(got->sdata, want->sdata, got->slen), "error code case %zu", i);
            const char* want_error = ray_error_msg();
            TEST_ASSERT_FMT(!strcmp(got_error, want_error ? want_error : ""), "error message case %zu: %s / %s", i, got_error, want_error ? want_error : "");
        } else {
            ray_t* a = ray_ser(got), *b = ray_ser(want);
            TEST_ASSERT(a && b && !RAY_IS_ERR(a) && !RAY_IS_ERR(b), "serialization");
            TEST_ASSERT_FMT(a->len == b->len && !memcmp(ray_data(a), ray_data(b), a->len), "result parity case %zu: %.1000s", i, src);
            drop(a); drop(b);
        }
        drop(got); drop(want);
    }
    PASS();
}

static test_result_t test_select_merge_scopes(void) {
    ray_t* t = ray_eval_str("(set T (table [a p] (list [0 1 1] [1.0 2.0 3.0])))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "fixture"); drop(t);
    ray_t* dict = ray_eval_str("(quote {from: (select {from:T where:(== a 1)}) total:(sum p)})");
    TEST_ASSERT(dict && !RAY_IS_ERR(dict), "AST");
    ray_t* before = ray_ser(dict);
    const char* names[] = {"a", "p", "==", "select"};
    for (int i = 0; i < 4; i++) {
        ray_env_push_scope();
        ray_t* v = ray_i64(42);
        ray_env_set_local(ray_sym_intern(names[i], strlen(names[i])), v);
        drop(v);
        ray_t* plan = ray_select_merge_plan(dict);
        TEST_ASSERT(!plan, "lexical/builtin shadow is a merge barrier");
        ray_env_pop_scope();
    }
    ray_t* plan = ray_select_merge_plan(dict);
    TEST_ASSERT(plan && !RAY_IS_ERR(plan), "ordinary query merges"); drop(plan);
    ray_t* after = ray_ser(dict);
    TEST_ASSERT(before && after && !RAY_IS_ERR(before) && !RAY_IS_ERR(after), "AST serialization");
    TEST_ASSERT(before->len == after->len && !memcmp(ray_data(before), ray_data(after), before->len), "rewrite leaves shared AST unchanged");
    drop(before); drop(after); drop(dict);
    /* A short alias chain can represent an exponentially large expression. */
    char source[2048];
    size_t len = (size_t)snprintf(source, sizeof(source), "(quote {from:(select {from:T q0:(+ p p)");
    for (int i = 1; i <= 20; i++)
        len += (size_t)snprintf(source + len, sizeof(source) - len, " q%d:(+ q%d q%d)", i, i - 1, i - 1);
    snprintf(source + len, sizeof(source) - len, "}) total:(sum q20)})");
    dict = ray_eval_str(source);
    TEST_ASSERT(dict && !RAY_IS_ERR(dict), "alias chain AST");
    plan = ray_select_merge_plan(dict);
    TEST_ASSERT(!plan, "bound alias expansion work"); drop(dict);
    ray_t* r = ray_eval_str("(do (set calls 0) (select {from:(select {from:(do (set calls (+ calls 1)) T) where:(== a 1)}) total:(sum p)}) calls)");
    TEST_ASSERT(r && !RAY_IS_ERR(r) && r->type == -RAY_I64 && r->i64 == 1, "source evaluates once");
    drop(r);
    PASS();
}

const test_entry_t select_merge_entries[] = {
    {"select_merge/cases", test_select_merge_cases, setup, teardown},
    {"select_merge/scopes", test_select_merge_scopes, setup, teardown},
    {NULL, NULL, NULL, NULL},
};
