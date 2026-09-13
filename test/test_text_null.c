/* Canonical text nulls must not depend on the optional HAS_NULLS metadata. */
#include "test.h"
#include "lang/eval.h"
#include "table/sym.h"
#include "vec/vec.h"
#include "ops/agg_engine.h"
#include "ops/idxop.h"
#include "ops/internal.h"
#include <string.h>

extern ray_runtime_t* __RUNTIME;
static void text_null_setup(void) { ray_runtime_create(0, NULL); }
static void text_null_teardown(void) { ray_runtime_destroy(__RUNTIME); }

static test_result_t expect_int(const char* expr, int64_t expected) {
    ray_t* r = ray_eval_str(expr);
    TEST_ASSERT_FMT(r && !RAY_IS_ERR(r), "eval failed: %s", expr);
    TEST_ASSERT_FMT(r->type == -RAY_I64 && r->i64 == expected,
                    "wrong integer result: %s (type=%d value=%lld expected=%lld)", expr,
                    r->type, (long long)r->i64, (long long)expected);
    ray_release(r);
    PASS();
}

static test_result_t text_null_operations(int8_t type) {
    ray_t* v = ray_eval_str(type == RAY_SYM
        ? "(as 'SYM (list \"\" \"beta\" \"alpha\" \"\"))"
        : "(as 'STR (list \"\" \"beta\" \"alpha\" \"\"))");
    TEST_ASSERT(v && !RAY_IS_ERR(v), "text input");
    v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
    TEST_ASSERT(ray_vec_may_have_nulls(v), "text payload may contain nulls");
    TEST_ASSERT(ray_vec_is_null(v, 0) && ray_vec_is_null(v, 3), "empty payloads are null");
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("text_input", 10), v), RAY_OK);
    ray_t* slice = ray_vec_slice(v, 0, 3);
    TEST_ASSERT(slice && !RAY_IS_ERR(slice), "text slice");
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("text_slice", 10), slice), RAY_OK);
    ray_release(slice);
    ray_release(v);

    const struct { const char* expr; int64_t value; } checks[] = {
        { "(sum (map nil? text_input))", 2 },
        { "(sum (map nil? text_slice))", 1 },
        { "(strlen (min text_slice))", 5 },
        { "(strlen (min text_input))", 5 },
        { "(strlen (max text_input))", 4 },
        { "(sum (map nil? (at text_input [3 1 0])))", 2 },
        { "(sum (map nil? (substr text_input 1 2)))", 2 },
        { "(count (distinct text_input))", 3 },
        { "(as 'I64 (nil? (at (asc text_input) 0)))", 1 },
        { "(as 'I64 (nil? (at (desc text_input) 3)))", 1 },
        { "(as 'I64 (nil? (min (at text_input [0 3]))))", 1 },
        { "(count (inner-join ['k] (table [k] (list text_input)) (table [k] (list text_input))))", 6 },
        { "(as 'I64 (nil? (at (at (select {from:(table [g v] (list [1 1] (at text_input [0 3]))) by:g m:(min v)}) 'm) 0)))", 1 },
        { "(count (select {from:(table [k v] (list text_input [1 2 3 4])) by:k s:(sum v)}))", 3 },
        { "(sum (at (select {from:(table [k v] (list text_input [1 2 3 4])) by:k s:(sum v)}) 's))", 10 },
        { "(sum (map nil? (at (select {from:(table [k v] (list text_input [1 2 3 4])) by:k s:(sum v)}) 'k)))", 1 },
        { "(count (select {from:(table [k] (list text_input)) where:(== k \"\")}))", 2 },
        { "(sum (map nil? (asc text_input)))", 2 },
        { "(sum (map nil? (desc text_input)))", 2 },
        { "(strlen (at (at (select {from:(table [g v] (list [1 1 1 1] text_input)) by:g m:(min v)}) 'm) 0))", 5 },
        { "(strlen (at (at (select {from:(table [g v] (list [1 1 1 1] text_input)) by:g f:(first v)}) 'f) 0))", 4 },
        { "(strlen (at (at (select {from:(table [g v] (list [1 1 1 1] text_input)) by:g l:(last v)}) 'l) 0))", 5 },
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        test_result_t r = expect_int(checks[i].expr, checks[i].value);
        if (r.status != TEST_PASS) return r;
    }
    PASS();
}
static test_result_t test_sym_payload_nulls(void) { return text_null_operations(RAY_SYM); }
static test_result_t test_str_payload_nulls(void) { return text_null_operations(RAY_STR); }

static test_result_t test_dense_sym_zero(void) {
    /* All widths, both metadata states: zero must remain a real group key. */
    const uint8_t widths[] = { RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64 };
    for (size_t w = 0; w < sizeof(widths); w++) {
        ray_t* v = ray_vec_new(RAY_SYM, 4);
        TEST_ASSERT(v && !RAY_IS_ERR(v), "sym vector");
        v->len = 4;
        v->attrs = widths[w];
        const int64_t ids[] = { 0, 1, 2, 0 };
        for (int i = 0; i < 4; i++) ray_write_sym(ray_data(v), i, ids[i], RAY_SYM, v->attrs);
        for (int flagged = 0; flagged < 2; flagged++) {
            if (flagged) v->attrs |= RAY_ATTR_HAS_NULLS;
            dense_plan_t plan;
            TEST_ASSERT(agg_dense_plan(&v, 1, NULL, 0, 4, &plan), "zero admits dense grouping");
            TEST_ASSERT(plan.ok, "dense plan valid");
        }
        ray_release(v);
    }
    PASS();
}

static test_result_t test_dense_symbol_gap(void) {
    /* Give the actual keys ids far from zero, as happens after importing
     * earlier columns of a wide CSV. Exercise parallel, mixed signed keys,
     * selection, and an all-empty input with both metadata states. */
    for (int i = 0; i < 5000; i++) {
        char name[32];
        int n = snprintf(name, sizeof(name), "gap_padding_%d", i);
        TEST_ASSERT(ray_sym_intern(name, (size_t)n) >= 0, "intern padding");
    }
    ray_t* t = ray_eval_str(
        "(table [k h v tm] (list (take [gap_a ' gap_b '] 70000) "
        "(take (as 'I16 [-2 -2 3 3]) 70000) "
        "(take [1.0 2.0 3.0 4.0] 70000) "
        "(as 'TIME (take [100 200 300 400] 70000))))");
    TEST_ASSERT(t && !RAY_IS_ERR(t), "large grouped fixture");
    TEST_ASSERT_EQ_I(ray_env_set(ray_sym_intern("gap_t", 5), t), RAY_OK);
    ray_t* key = ray_table_get_col(t, ray_sym_intern("k", 1));
    TEST_ASSERT(key && key->type == RAY_SYM, "sym key");
    const struct { const char* expr; int64_t value; } checks[] = {
        { "(count (select {from:gap_t by:k s:(sum v) m:(max tm)}))", 3 },
        { "(count (select {from:gap_t by:[k h] s:(sum v) m:(max tm)}))", 4 },
        { "(count (select {from:gap_t by:k s:(sum v) m:(max tm) where:(== h -2)}))", 2 },
        { "(count (select {from:gap_t by:k s:(sum v) m:(max tm) where:(== k ')}))", 1 },
        { "(as 'I64 (sum (at (select {from:gap_t by:[k h] s:(sum v) m:(max tm)}) 's)))", 175000 },
        { "(as 'I64 (sum (at (select {from:gap_t by:k s:(sum v) m:(max tm) where:(== k ')}) 's)))", 105000 },
        { "(sum (map nil? (at (select {from:gap_t by:k s:(sum v) m:(max tm)}) 'k)))", 1 },
    };
    for (int flagged = 0; flagged < 2; flagged++) {
        key->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
        if (flagged) key->attrs |= RAY_ATTR_HAS_NULLS;
        for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
            test_result_t r = expect_int(checks[i].expr, checks[i].value);
            if (r.status != TEST_PASS) { ray_release(t); return r; }
        }
    }
    ray_release(t);
    PASS();
}


/* Assert admission as well as values: conservative text null gates must not
 * silently turn indexed predicates or expression compilation into scans. */
static test_result_t test_sym_hash_routes(void) {
    for (int nullable = 0; nullable < 2; nullable++) {
        for (int flagged = 0; flagged < 2; flagged++) {
            ray_t* v = ray_eval_str(nullable ? "[' beta alpha ']" : "[alpha beta alpha alpha]");
            TEST_ASSERT(v && !RAY_IS_ERR(v), "symbol fixture");
            v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
            if (flagged) v->attrs |= RAY_ATTR_HAS_NULLS;
            ray_t* attached = ray_index_attach_hash(&v);
            TEST_ASSERT(attached && !RAY_IS_ERR(attached), "hash attach");
            int64_t beta = ray_sym_intern("beta", 4);
            TEST_ASSERT_EQ_I(ray_index_find_row(v, beta), 1);
            TEST_ASSERT_EQ_I(ray_index_find_row(v, 0), -2); /* null scan fallback */
            ray_t* keys = ray_eval_str("[beta]");
            ray_t* sel = ray_index_in_rowsel(v, keys);
            TEST_ASSERT(sel && !RAY_IS_ERR(sel), "nonnull IN uses index");
            ray_release(sel);
            ray_idx_slice_t* slices = NULL;
            ray_t* hdr = NULL;
            TEST_ASSERT_EQ_I(ray_index_sym_slices(v, keys, &slices, &hdr), 1);
            TEST_ASSERT_EQ_I(slices[0].n, 1);
            ray_free(hdr);
            ray_release(keys);
            keys = ray_eval_str("[beta ']");
            TEST_ASSERT(ray_index_in_rowsel(v, keys) == NULL, "null IN falls back");
            TEST_ASSERT_EQ_I(ray_index_sym_slices(v, keys, &slices, &hdr), -1);
            ray_release(keys);

            ray_t* tbl = ray_table_new(1);
            tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), v);
            ray_release(v);
            for (int empty = 0; empty < 2; empty++) {
                ray_graph_t* g = ray_graph_new(tbl);
                ray_t* atom = ray_sym(empty ? 0 : beta);
                ray_op_t* pred = ray_eq(g, ray_scan(g, "k"), ray_const_atom(g, atom));
                ray_release(atom);
                ray_op_t* filter = ray_filter(g, ray_const_table(g, tbl), pred);
                uint64_t before = ray_idx_hits[IDX_SITE_FILTER_HASH];
                ray_t* out = ray_execute(g, filter);
                TEST_ASSERT(out && !RAY_IS_ERR(out), "indexed filter result");
                TEST_ASSERT_EQ_I(ray_table_nrows(out), empty ? (nullable ? 2 : 0) : 1);
                if (!empty)
                    TEST_ASSERT(ray_idx_hits[IDX_SITE_FILTER_HASH] > before, "symbol filter uses hash index");
                ray_release(out);
                ray_graph_free(g);
            }
            ray_release(tbl);
        }
    }
    PASS();
}

static test_result_t test_sym_expr_admission(void) {
    for (int nullable = 0; nullable < 2; nullable++) {
        for (int flagged = 0; flagged < 2; flagged++) {
            ray_t* v = ray_eval_str(nullable ? "[' beta alpha]" : "[alpha beta alpha]");
            TEST_ASSERT(v && !RAY_IS_ERR(v), "symbol fixture");
            v->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
            if (flagged) v->attrs |= RAY_ATTR_HAS_NULLS;
            ray_t* tbl = ray_table_new(1);
            tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), v);
            ray_release(v);
            ray_graph_t* g = ray_graph_new(tbl);
            ray_op_t* pred = ray_eq(g, ray_scan(g, "k"), ray_const_str(g, "beta", 4));
            ray_expr_t expr;
            TEST_ASSERT_EQ_I(expr_compile(g, tbl, pred, &expr), !nullable);
            ray_graph_free(g);
            ray_release(tbl);
        }
    }
    PASS();
}

const test_entry_t text_null_entries[] = {
    { "text_null/sym_hash_routes", test_sym_hash_routes, text_null_setup, text_null_teardown },
    { "text_null/sym_expr_admission", test_sym_expr_admission, text_null_setup, text_null_teardown },
    { "text_null/sym_without_flag", test_sym_payload_nulls, text_null_setup, text_null_teardown },
    { "text_null/str_without_flag", test_str_payload_nulls, text_null_setup, text_null_teardown },
    { "text_null/dense_symbol_gap", test_dense_symbol_gap, text_null_setup, text_null_teardown },
    { "text_null/dense_sym_zero", test_dense_sym_zero, text_null_setup, text_null_teardown },
    { NULL, NULL, NULL, NULL },
};
