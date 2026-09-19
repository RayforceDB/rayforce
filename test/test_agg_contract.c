/* Scalar/grouped semantic contracts, independent of optimized admission.
 * Baseline census: docs/aggregation-type-census.csv. Extend this table when
 * language semantics change; adding a vtable alone must not change it. */
#include "test.h"
#include "test_rfl.h"
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "core/pool.h"
#include "core/platform.h"
#include "mem/heap.h"
#include "ops/fused_pred.h"
#include "ops/cdfuse.h"
#include "ops/internal.h"
#include "lang/internal.h"
#include "lang/env.h"
#include <math.h>

static ray_runtime_t* contract_runtime;
static uint32_t contract_cores;
static void contract_setup(void) {
    contract_runtime = ray_runtime_create(0, NULL);
    contract_cores = ray_pool_total_workers(ray_pool_get());
    ray_agg_engine_v2 = true;
    agg_route_reset();
}
static void contract_teardown(void) {
    ray_agg_engine_v2 = true;
    ray_runtime_destroy(contract_runtime);
    if (ray_pool_total_workers(ray_pool_get()) != contract_cores) {
        ray_pool_destroy();
        ray_pool_init_total(contract_cores);
    }
}

static const char* const unary_names[] = {
    "count", "sum", "avg", "min", "max", "first", "last", "prod",
    "var", "var_pop", "stddev", "stddev_pop", "all", "any", "med"
};
static const struct {
    int8_t input;
    int8_t output[15];  /* 0: deliberately illegal, otherwise logical vector type */
} unary_contracts[] = {
    { RAY_BOOL, { RAY_I64, RAY_I64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_BOOL, RAY_BOOL, RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_U8, { RAY_I64, RAY_I64, RAY_F64, RAY_U8, RAY_U8, RAY_U8, RAY_U8, RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_I16, { RAY_I64, RAY_I64, RAY_F64, RAY_I16, RAY_I16, RAY_I16, RAY_I16, RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_I32, { RAY_I64, RAY_I64, RAY_F64, RAY_I32, RAY_I32, RAY_I32, RAY_I32, RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_I64, { RAY_I64, RAY_I64, RAY_F64, RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_F32, { RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_F64, { RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
    { RAY_DATE, { RAY_I64, 0, RAY_F64, RAY_DATE, RAY_DATE, RAY_DATE, RAY_DATE, 0, RAY_F64, RAY_F64, RAY_F64, RAY_F64, 0, 0, RAY_F64 } },
    { RAY_TIME, { RAY_I64, RAY_TIME, RAY_F64, RAY_TIME, RAY_TIME, RAY_TIME, RAY_TIME, 0, RAY_F64, RAY_F64, RAY_F64, RAY_F64, 0, 0, RAY_F64 } },
    { RAY_TIMESTAMP, { RAY_I64, 0, RAY_F64, RAY_TIMESTAMP, RAY_TIMESTAMP, RAY_TIMESTAMP, RAY_TIMESTAMP, 0, RAY_F64, RAY_F64, RAY_F64, RAY_F64, 0, 0, RAY_F64 } },
    { RAY_GUID, { RAY_I64, 0, 0, RAY_GUID, RAY_GUID, RAY_GUID, RAY_GUID, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { RAY_SYM, { RAY_I64, 0, 0, RAY_SYM, RAY_SYM, RAY_SYM, RAY_SYM, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { RAY_STR, { RAY_I64, 0, 0, RAY_STR, RAY_STR, RAY_STR, RAY_STR, 0, 0, 0, 0, 0, 0, 0, 0 } },
    { RAY_LIST, { RAY_I64, RAY_I64, RAY_F64, RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_I64, RAY_F64, RAY_F64, RAY_F64, RAY_F64, RAY_BOOL, RAY_BOOL, RAY_F64 } },
};

static ray_t* contract_fixture(int8_t type) {
    if (type == RAY_GUID) return ray_eval_str("(guid 4)");
    if (type == RAY_SYM) return ray_eval_str("['b 'a 'b 'c]");
    if (type == RAY_STR) return ray_eval_str("[\"b\" \"a\" \"b\" \"c\"]");
    if (type == RAY_LIST) return ray_eval_str("(list 1 2 3 4)");
    ray_t* v = ray_vec_new(type, 4);
    if (!v || RAY_IS_ERR(v)) return v;
    v->len = 4;
    for (int i = 0; i < 4; i++) {
        switch (type) {
            case RAY_BOOL: ((uint8_t*)ray_data(v))[i] = i % 2; break;
            case RAY_U8: ((uint8_t*)ray_data(v))[i] = i + 1; break;
            case RAY_I16: ((int16_t*)ray_data(v))[i] = i + 1; break;
            case RAY_I32: case RAY_DATE: case RAY_TIME:
                ((int32_t*)ray_data(v))[i] = i + 1; break;
            case RAY_I64: case RAY_TIMESTAMP:
                ((int64_t*)ray_data(v))[i] = i + 1; break;
            case RAY_F32: ((float*)ray_data(v))[i] = (float)i + 1.25f; break;
            case RAY_F64: ((double*)ray_data(v))[i] = (double)i + 1.25; break;
        }
    }
    return v;
}

static test_result_t test_unary_contracts(void) {
    for (size_t t = 0; t < sizeof(unary_contracts)/sizeof(unary_contracts[0]); t++) {
        const int8_t type = unary_contracts[t].input;
        ray_t* v = contract_fixture(type);
        TEST_ASSERT_NOT_NULL(v);
        TEST_ASSERT_FALSE(RAY_IS_ERR(v));
        TEST_ASSERT_EQ_I(v->type, type);
        ray_env_set(ray_sym_intern("v", 1), v);
        ray_release(v);
        ray_t* init = ray_eval_str("(set t (table [k v] (list [0 0 1 1] v)))");
        TEST_ASSERT_FALSE(RAY_IS_ERR(init)); ray_release(init);
        for (size_t a = 0; a < sizeof(unary_names)/sizeof(unary_names[0]); a++) {
            char source[256];
            snprintf(source, sizeof(source),
                "(at (select {from:t by:k s:(%s v) asc:k}) 's)", unary_names[a]);
            ray_t* got = ray_eval_str(source);
            int8_t output = unary_contracts[t].output[a];
            if (!output) {
                bool error = RAY_IS_ERR(got);
                if (error) ray_error_free(got); else ray_release(got);
                TEST_ASSERT_FMT(error, "%s(%s) grouped must reject input",
                                unary_names[a], ray_type_name(type));
                snprintf(source, sizeof(source), "(%s v)", unary_names[a]);
                got = ray_eval_str(source);
                error = RAY_IS_ERR(got);
                if (error) ray_error_free(got); else ray_release(got);
                TEST_ASSERT_FMT(error, "%s(%s) scalar must reject input",
                                unary_names[a], ray_type_name(type));
                continue;
            }
            TEST_ASSERT_FMT(got && !RAY_IS_ERR(got), "%s(%s) grouped failed",
                            unary_names[a], ray_type_name(type));
            TEST_ASSERT_FMT(got->type == output && got->len == 2,
                            "%s(%s): got %s, expected %s[2]",
                            unary_names[a], ray_type_name(type),
                            ray_type_name(got->type), ray_type_name(output));
            for (int64_t group = 0; group < 2; group++) {
                snprintf(source, sizeof(source), "(%s (at v [%lld %lld]))",
                         unary_names[a], (long long)(2*group), (long long)(2*group+1));
                ray_t* want = ray_eval_str(source);
                TEST_ASSERT_FMT(want && !RAY_IS_ERR(want), "%s scalar failed", source);
                ray_t* idx = ray_i64(group);
                ray_t* cell = ray_at_fn(got, idx); ray_release(idx);
                TEST_ASSERT_FMT(cell->type == want->type, "%s: grouped/scalar type mismatch", source);
                bool equal;
                if (cell->type == -RAY_F64) {
                    equal = (isnan(cell->f64) && isnan(want->f64)) ||
                            fabs(cell->f64 - want->f64) <= 1e-12;
                } else {
                    ray_t* eq = ray_eq_fn(cell, want);
                    equal = eq && eq->type == -RAY_BOOL && eq->b8;
                    ray_release(eq);
                }
                ray_release(cell); ray_release(want);
                TEST_ASSERT_FMT(equal, "%s(%s), group %lld differs from scalar slice",
                                unary_names[a], ray_type_name(type), (long long)group);
            }
            ray_release(got);
        }
    }
    PASS();
}

/* Exercise the direct serial emitter against scalar language semantics,
 * including states with no rows, no live rows, and wrapped/null results. */
static test_result_t test_native_streaming_output(void) {
    const uint16_t ops[] = { OP_COUNT, OP_SUM, OP_AVG, OP_MIN, OP_MAX, OP_FIRST,
        OP_LAST, OP_PROD, OP_VAR, OP_VAR_POP, OP_STDDEV, OP_STDDEV_POP, OP_ALL, OP_ANY, OP_MEDIAN };
    const uint32_t gids[] = { 0, 0, 1, 1 };
    for (size_t t = 0; t < 10; t++) {
        int8_t type = unary_contracts[t].input;
        for (int shape = 0; shape < 3; shape++) {
            ray_t* v = contract_fixture(type);
            if (shape == 1 && type != RAY_BOOL && type != RAY_U8) {
                for (int i = 0; i < 4; i++) ray_vec_set_null(v, i, true);
            } else if (shape == 2) {
                if (type == RAY_I64) {
                    int64_t* d = ray_data(v);
                    d[0] = INT64_MAX; d[1] = 1; d[2] = INT64_MAX; d[3] = 2;
                } else if (type == RAY_TIME) {
                    int32_t* d = ray_data(v);
                    d[0] = INT32_MAX; d[1] = 1; d[2] = INT32_MAX; d[3] = 2;
                } else if (type == RAY_F64) {
                    double* d = ray_data(v);
                    d[0] = 1e308; d[1] = 1e308; d[2] = INFINITY; d[3] = -INFINITY;
                }
            }
            ray_env_set(ray_sym_intern("v", 1), v);
            for (size_t a = 0; a < sizeof(ops)/sizeof(ops[0]); a++) {
                /* Integer grouped statistics intentionally retain wrapped
                 * sum-of-squares semantics; scalar variance uses another
                 * algorithm. Overflow here tests sum/product output only. */
                if (shape == 2 && ops[a] != OP_SUM && ops[a] != OP_PROD) continue;
                const agg_vtable_t* vt = agg_resolve(ops[a], type);
                if (!vt || vt->kind != ACC_STREAMING) continue;
                ray_t* out = agg_run_one(vt, v, gids, 4, 3, 0);
                TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
                for (int g = 0; g < 3; g++) {
                    /* Non-nullable extrema have no representable empty vector
                     * cell; real grouping never emits an absent group. */
                    if (g == 2 && (type == RAY_BOOL || type == RAY_U8) &&
                            (ops[a] == OP_MIN || ops[a] == OP_MAX)) continue;
                    char source[128];
                    if (g == 2) snprintf(source, sizeof(source), "(%s (take v 0))", unary_names[a]);
                    else snprintf(source, sizeof(source), "(%s (at v [%d %d]))", unary_names[a], 2*g, 2*g+1);
                    /* Grouped product is null when there are no live rows. */
                    ray_t* want = ops[a] == OP_PROD && (g == 2 ||
                        (shape == 1 && type != RAY_BOOL && type != RAY_U8))
                        ? ray_typed_null(-vt->out_type) : ray_eval_str(source);
                    TEST_ASSERT_NOT_NULL(want); TEST_ASSERT_FALSE(RAY_IS_ERR(want));
                    ray_t* index = ray_i64(g);
                    ray_t* got = ray_at_fn(out, index); ray_release(index);
                    TEST_ASSERT_EQ_I(got->type, want->type);
                    bool same;
                    if (want->type == -RAY_F64)
                        same = (isnan(got->f64) && isnan(want->f64)) ||
                            fabs(got->f64 - want->f64) <= 1e-12 * fmax(1.0, fabs(want->f64));
                    else {
                        ray_t* gs = ray_fmt(got, 0); ray_t* ws = ray_fmt(want, 0);
                        same = strcmp(ray_str_ptr(gs), ray_str_ptr(ws)) == 0;
                        ray_release(gs); ray_release(ws);
                    }
                    if (RAY_ATOM_IS_NULL(want)) TEST_ASSERT_TRUE(out->attrs & RAY_ATTR_HAS_NULLS);
                    ray_release(got); ray_release(want);
                    TEST_ASSERT_FMT(same, "native %s/%s shape %d group %d", unary_names[a], ray_type_name(type), shape, g);
                }
                ray_release(out);
            }
            ray_release(v);
        }
    }
    PASS();
}

/* Registry presence and execution admission are different contracts: buffered
 * I64/F64 vtables exist today, but must not silently pass streaming admission. */
static test_result_t test_registry_admission_contracts(void) {
    const uint16_t operations[] = {
        OP_COUNT, OP_SUM, OP_MIN, OP_MAX, OP_AVG, OP_VAR, OP_VAR_POP,
        OP_STDDEV, OP_STDDEV_POP, OP_FIRST, OP_LAST, OP_PROD, OP_ALL, OP_ANY,
        OP_MEDIAN, OP_TOP_N, OP_BOT_N, OP_QUANTILE, OP_MODE,
        OP_PEARSON_CORR, OP_COV, OP_SCOV, OP_WSUM, OP_WAVG
    };
    for (size_t t = 0; t < sizeof(unary_contracts)/sizeof(unary_contracts[0]); t++) {
        int8_t type = unary_contracts[t].input;
        ray_t* v = contract_fixture(type);
        TEST_ASSERT_NOT_NULL(v);
        ray_t* k = ray_eval_str("[0 0 1 1]");
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), k);
        tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), v);
        ray_release(k); ray_release(v);
        for (size_t a = 0; a < sizeof(operations)/sizeof(operations[0]); a++) {
            uint16_t op = operations[a];
            bool numeric = type == RAY_BOOL || type == RAY_U8 || type == RAY_I16 ||
                type == RAY_I32 || type == RAY_I64 || type == RAY_F32 || type == RAY_F64;
            bool wide_numeric = type == RAY_I64 || type == RAY_F64;
            bool binary = agg_is_binary_agg(op);
            bool buffered = op == OP_MEDIAN || op == OP_TOP_N || op == OP_BOT_N;
            bool temporal = type == RAY_DATE || type == RAY_TIME || type == RAY_TIMESTAMP;
            bool registered = op == OP_COUNT ||
                ((op == OP_MIN || op == OP_MAX || op == OP_AVG ||
                  op == OP_VAR || op == OP_VAR_POP || op == OP_STDDEV ||
                  op == OP_STDDEV_POP) && (numeric || temporal)) ||
                (op == OP_SUM && (numeric || type == RAY_TIME)) ||
                (op == OP_PROD && numeric) ||
                (buffered && wide_numeric) ||
                ((op == OP_ALL || op == OP_ANY || binary) && numeric);
            const agg_vtable_t* vt = agg_resolve(op, type);
            TEST_ASSERT_FMT((vt != NULL) == registered, "registry %u/%s changed", op, ray_type_name(type));
            if (vt && vt->kind == ACC_STREAMING)
                TEST_ASSERT_FMT(vt->finalize_value != NULL,
                                "streaming %u/%s must emit native values", op, ray_type_name(type));
            ray_graph_t* graph = ray_graph_new(tbl);
            ray_op_t* keys[] = { ray_scan(graph, "k") };
            ray_op_t* ins[] = { ray_scan(graph, "v") };
            int64_t param[] = { op == OP_TOP_N || op == OP_BOT_N ? 2 : 0 };
            ray_op_t* group = ray_group_build(graph, keys, 1, &op, ins,
                                               binary ? ins : NULL, param, 1);
            bool indexed = ((op == OP_FIRST || op == OP_LAST) && (numeric || temporal || type == RAY_GUID || type == RAY_SYM || type == RAY_STR || type == RAY_LIST)) ||
                ((op == OP_MIN || op == OP_MAX) && (type == RAY_GUID || type == RAY_SYM || type == RAY_STR)) ||
                ((op == OP_MEDIAN || op == OP_QUANTILE) && (numeric || temporal)) ||
                ((op == OP_MODE || op == OP_TOP_N || op == OP_BOT_N) && (numeric || temporal || type == RAY_GUID || type == RAY_SYM || type == RAY_STR));
            agg_v2_reason_t want = indexed ? AGG_V2_ADMITTED : !registered ? AGG_V2_AGG_TYPE
                : buffered ? AGG_V2_BUFFERED : AGG_V2_ADMITTED;
            TEST_ASSERT_FMT(agg_v2_admission(graph, group, tbl) == want,
                            "admission %u/%s differs", op, ray_type_name(type));
            TEST_ASSERT_EQ_I(agg_v2_can_handle(graph, group, tbl), want == AGG_V2_ADMITTED);
            ray_graph_free(graph);
        }
        ray_release(tbl);
    }
    /* Pure inspection must not fabricate any execution. */
    agg_route_stats_t stats = agg_route_stats();
    for (int i = 0; i < AGG_ROUTE_COUNT; i++) TEST_ASSERT_EQ_I(stats.routes[i], 0);
    PASS();
}

/* A single-expression `by:` over one SYM column with few distinct symbols
 * is evaluated once per symbol (agg_route_stats().key_domain_evals); a
 * positional expression, a two-column expression, a shadowed builtin and a
 * table below the row gate all take the row-wise key. */
static test_result_t test_derived_key_per_symbol_route(void) {
    static const char* const setup =
        "(set i (til 8192)) "
        "(set s (as 'SYMBOL (map (fn [k] (format \"h%.x\" (% k 16))) i))) "
        "(set v (as 'F64 (% i 7))) "
        "(set T (table [s v] (list s v))) "
        "(set T3 (take T 3000))";
    ray_t* r = ray_eval_str(setup);
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); ray_release(r);
    const struct { const char* q; uint64_t evals; } cases[] = {
        { "(select {from: T by: (substr s 0 2) c: (count v)})", 1 },
        { "(select {from: T by: (let p (str-find s \".\") (if (> p 1) (substr s 0 p) s)) c: (count v)})", 1 },
        { "(select {from: T by: (differ s) c: (count v)})", 0 },
        { "(select {from: T by: (if (> v 3) (substr s 0 2) s) c: (count v)})", 0 },
        { "(select {from: T3 by: (substr s 0 2) c: (count v)})", 0 },
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        agg_route_reset();
        ray_t* out = ray_eval_str(cases[c].q);
        TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
        ray_release(out);
        TEST_ASSERT_EQ_I(agg_route_stats().key_domain_evals, cases[c].evals);
    }
    /* A user lambda shadowing `substr` is what the compiled key would call. */
    r = ray_eval_str("(set substr (fn [x a b] x))");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); ray_release(r);
    agg_route_reset();
    r = ray_eval_str("(select {from: T by: (substr s 0 2) c: (count v)})");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); ray_release(r);
    TEST_ASSERT_EQ_I(agg_route_stats().key_domain_evals, 0);
    PASS();
}

static test_result_t test_group_routes_and_bool_outputs(void) {
    /* Route expectations below use two cores regardless of harness settings. */
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(2), RAY_OK);
    /* 65536 rows exercises parallel finalization with one output per group.
     * all/any BOOL outputs previously wrote at 8-byte strides into byte vectors. */
    const struct { int64_t n; int mode; agg_route_t route; } cases[] = {
        { 32, 0, AGG_ROUTE_V2_SERIAL_DENSE },
        { 32, 1, AGG_ROUTE_V2_SERIAL_HASH },
        { RAY_PARALLEL_THRESHOLD, 0, AGG_ROUTE_V2_DENSE },
        { RAY_PARALLEL_THRESHOLD, 1, AGG_ROUTE_V2_RADIX },
        { RAY_PARALLEL_THRESHOLD, 2, AGG_ROUTE_V2_DENSE },
        { RAY_PARALLEL_THRESHOLD, 3, AGG_ROUTE_V2_DENSE },
        { RAY_PARALLEL_THRESHOLD, 4, AGG_ROUTE_V2_RADIX },
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        int64_t n = cases[c].n;
        ray_t* k = ray_vec_new(RAY_I64, n); k->len = n;
        ray_t* v = ray_vec_new(RAY_BOOL, n); v->len = n;
        for (int64_t i = 0; i < n; i++) {
            int64_t key = cases[c].mode >= 3 ? i : i % 4;
            ((int64_t*)ray_data(k))[i] = (cases[c].mode == 1 || cases[c].mode == 4) ? key * 1000000 : key;
            ((uint8_t*)ray_data(v))[i] = key % 2;
        }
        if (cases[c].mode == 2) ray_vec_set_null(k, 0, true);
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), k);
        tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), v);
        ray_release(k); ray_release(v);
        ray_graph_t* graph = ray_graph_new(tbl);
        ray_op_t* keys[] = { ray_scan(graph, "k") };
        ray_op_t* val = ray_scan(graph, "v");
        ray_op_t* inputs[] = { val, val };
        uint16_t ops[] = { OP_ALL, OP_ANY };
        ray_op_t* group = ray_group(graph, keys, 1, ops, inputs, 2);
        agg_route_reset();
        ray_t* out = ray_execute(graph, group);
        TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
        agg_route_stats_t stats = agg_route_stats();
        TEST_ASSERT_EQ_I(stats.routes[cases[c].route], 1);
        TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_LEGACY], 0);
        TEST_ASSERT_EQ_I(stats.nullable_key, cases[c].mode == 2);
        TEST_ASSERT_FALSE(stats.dense_worker_budget);
        ray_t* out_keys = ray_table_get_col_idx(out, 0);
        for (int a = 1; a <= 2; a++) {
            ray_t* col = ray_table_get_col_idx(out, a);
            TEST_ASSERT_EQ_I(col->type, RAY_BOOL);
            for (int64_t i = 0; i < col->len; i++) {
                int64_t key = ((int64_t*)ray_data(out_keys))[i];
                if (key == NULL_I64) key = 0;
                if (cases[c].mode == 1 || cases[c].mode == 4) key /= 1000000;
                TEST_ASSERT_EQ_I(((uint8_t*)ray_data(col))[i], key % 2);
            }
        }
        ray_release(out); ray_graph_free(graph); ray_release(tbl);
    }
    agg_route_reset();
    ray_t* r = ray_eval_str("(select {from:(table [k v] (list [0 0 1 1] (as 'TIME [1 2 3 4]))) by:k s:(min v)})");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r)); ray_release(r);
    agg_route_stats_t stats = agg_route_stats();
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_SERIAL_DENSE], 1);
    TEST_ASSERT_EQ_I(stats.last_v2_reason, AGG_V2_ADMITTED);
    /* Shared extrema keep one state per group even when every input range
     * visits the whole key domain. Pool size no longer multiplies state. */
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(8), RAY_OK);
    ray_t* setup = ray_eval_str("(set traffic_i (til 1000000)) (set traffic_g (% (* traffic_i 17) 80000)) (set traffic_t (table [k v] (list (as 'I32 (+ (* traffic_g 2) (div traffic_g 4))) (as 'TIME traffic_g))))");
    TEST_ASSERT_NOT_NULL(setup); TEST_ASSERT_FALSE(RAY_IS_ERR(setup)); ray_release(setup);
    agg_route_reset();
    r = ray_eval_str("(select {from:traffic_t by:k s:(min v)})");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    stats = agg_route_stats();
    TEST_ASSERT_TRUE(stats.dense_plan_available);
    TEST_ASSERT_FALSE(stats.dense_worker_budget);
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_DENSE], 1);
    TEST_ASSERT_EQ_I(stats.dense_strategy, AGG_DENSE_PARTITIONED);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 80000);
    ray_release(r);
    /* A larger machine must not lose dense execution solely because a slab
     * for every physical worker would exceed the budget. Logical task IDs
     * own the slabs, including selected-row tasks run by a larger pool. */
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(20), RAY_OK);
    setup = ray_eval_str("(set traffic_g (div traffic_i 13)) (set traffic_t (table [k v] (list (as 'I32 (+ (* traffic_g 2) (div traffic_g 4))) (as 'TIME traffic_g))))");
    TEST_ASSERT_NOT_NULL(setup); TEST_ASSERT_FALSE(RAY_IS_ERR(setup)); ray_release(setup);
    const char* queries[] = {
        "(select {from:traffic_t by:k s:(min v)})",
        "(select {from:traffic_t by:k s:(min v) where:(> v 100)})"
    };
    for (int selected = 0; selected < 2; selected++) {
        agg_route_reset(); r = ray_eval_str(queries[selected]);
        TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r));
        stats = agg_route_stats();
        TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_DENSE], 1);
        TEST_ASSERT_TRUE(stats.dense_tasks > 0);
        TEST_ASSERT_EQ_I(stats.dense_strategy, AGG_DENSE_PARTITIONED);
        TEST_ASSERT_TRUE(stats.dense_tasks <= RAY_POOL_INIT_TASKS);
        TEST_ASSERT_TRUE(stats.dense_local_slots < 400000);
        TEST_ASSERT_EQ_I(ray_pool_total_workers(ray_pool_get()), 20);
        TEST_ASSERT_EQ_I(ray_table_nrows(r), 76924 - selected * 101);
        ray_t* keys = ray_table_get_col_idx(r, 0);
        ray_t* values = ray_table_get_col_idx(r, 1);
        TEST_ASSERT_EQ_I(values->type, RAY_TIME);
        for (int64_t row = 0; row < values->len; row++) {
            int32_t value = ((int32_t*)ray_data(values))[row];
            TEST_ASSERT_EQ_I(((int32_t*)ray_data(keys))[row], value * 2 + value / 4);
            TEST_ASSERT_TRUE(!selected || value > 100);
        }
        ray_release(r);
    }
    PASS();
}

static test_result_t test_narrow_extrema_limits(void) {
    const int8_t types[] = {RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_DATE, RAY_TIME};
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        for (int maximum = 0; maximum < 2; maximum++) {
            int8_t type = types[t];
            int width = type == RAY_BOOL || type == RAY_U8 ? 1 : type == RAY_I16 ? 2 : 4;
            int32_t expected = width == 1 ? (type == RAY_BOOL ? 1 : 255)
                : maximum ? (width == 2 ? INT16_MIN + 1 : INT32_MIN + 1)
                : (width == 2 ? INT16_MAX : INT32_MAX);
            ray_t* val = ray_vec_new(type, 2);
            val->len = 2;
            if (width == 1) { ((uint8_t*)ray_data(val))[0] = expected; ((uint8_t*)ray_data(val))[1] = expected; }
            else if (width == 2) { ((int16_t*)ray_data(val))[0] = expected; ((int16_t*)ray_data(val))[1] = NULL_I16; }
            else { ((int32_t*)ray_data(val))[0] = expected; ((int32_t*)ray_data(val))[1] = NULL_I32; }
            const agg_vtable_t* vt = agg_resolve(maximum ? OP_MAX : OP_MIN, type);
            TEST_ASSERT_EQ_I(vt->state_size, 8);
            uint64_t states[2];
            vt->init(&states[0]); vt->init(&states[1]);
            uint32_t gids[] = {0, 0};
            ray_valid_t valid = {ray_data(val), type, width != 1};
            vt->update_batch(states, vt->state_size, gids, ray_data(val), &valid, 2, NULL);
            vt->merge(&states[1], &states[0], NULL);
            union { uint64_t align[2]; uint8_t bytes[16]; } output;
            memset(output.bytes, 0xa5, sizeof(output.bytes));
            TEST_ASSERT_FALSE(vt->finalize_value(&states[1], output.bytes));
            int64_t got = width == 1 ? output.bytes[0] : width == 2
                ? *(int16_t*)output.bytes : *(int32_t*)output.bytes;
            TEST_ASSERT_EQ_I(got, expected);
            for (int i = width; i < 16; i++) TEST_ASSERT_EQ_I(output.bytes[i], 0xa5);
            vt->init(&states[0]);
            TEST_ASSERT_TRUE(vt->finalize_value(&states[0], output.bytes));
            ray_release(val);
        }
    }
    PASS();
}

/* Compare shared and partitioned execution with an independent row oracle.
 * A frequent null key stresses contention/skew; one live key has no valid
 * aggregate values. Native widths, mixed COUNT, binary inputs, and repeated
 * scratch reuse all pass through the common output stage. */
static test_result_t test_dense_strategies(void) {
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    const int8_t types[] = {RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_DATE, RAY_TIME, RAY_F32, RAY_F64, RAY_I64, RAY_TIMESTAMP};
    const int64_t n = 262144;
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        int8_t type = types[t];
        bool floating = type == RAY_F32 || type == RAY_F64;
        bool wide_int = type == RAY_I64 || type == RAY_TIMESTAMP;
        int width = type == RAY_BOOL || type == RAY_U8 ? 1 : type == RAY_I16 ? 2 : type == RAY_F64 || wide_int ? 8 : 4;
        ray_t* key = ray_vec_new(RAY_I32, n);
        ray_t* val = ray_vec_new(type, n);
        key->len = val->len = n;
        if (type == RAY_LIST) memset(ray_data(key), 0, (size_t)n * sizeof(ray_t*));
        key->attrs |= RAY_ATTR_HAS_NULLS;
        if (width != 1) val->attrs |= RAY_ATTR_HAS_NULLS;
        int64_t mn[8193], mx[8193], counts[8193] = {0}, sums[8193] = {0};
        for (int k = 0; k <= 8192; k++) { mn[k] = INT64_MAX; mx[k] = INT64_MIN; }
        for (int64_t i = 0; i < n; i++) {
            int32_t k = i / 2 % 8192 - 4096;
            bool null_key = i % 3 == 0;
            int group = null_key ? 8192 : k + 4096;
            bool null_value = width != 1 && (i % 97 == 0 || k == -1000);
            int32_t value = width == 1 ? i % (type == RAY_BOOL ? 2 : 251) : i % 10007 - 5000;
            ((int32_t*)ray_data(key))[i] = null_key ? NULL_I32 : k * 8;
            if (type == RAY_F32) ((float*)ray_data(val))[i] = null_value ? NAN : value;
            else if (type == RAY_F64) ((double*)ray_data(val))[i] = null_value ? NAN : value;
            else if (width == 1) ((uint8_t*)ray_data(val))[i] = value;
            else if (width == 2) ((int16_t*)ray_data(val))[i] = null_value ? NULL_I16 : value;
            else if (wide_int) ((int64_t*)ray_data(val))[i] = null_value ? NULL_I64 : value;
            else ((int32_t*)ray_data(val))[i] = null_value ? NULL_I32 : value;
            counts[group]++;
            if (!null_value) {
                sums[group] += value;
                if (value < mn[group]) mn[group] = value;
                if (value > mx[group]) mx[group] = value;
            }
        }
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), key);
        tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), val);
        ray_release(key); ray_release(val);
        for (int mode = 0; mode < 4; mode++) {
            if (mode == 2 && type != RAY_I32) continue;
            if (mode == 3 && (type == RAY_DATE || type == RAY_TIMESTAMP)) continue;
            for (int repeat = 0; repeat < 2; repeat++) {
                ray_graph_t* graph = ray_graph_new(tbl);
                ray_op_t* keys[] = {ray_scan(graph, "k")};
                ray_op_t* v = ray_scan(graph, "v");
                ray_op_t* inputs[] = {v, v, v};
                uint16_t ops[] = {OP_MIN, OP_MAX, OP_COUNT};
                ray_op_t* group;
                if (mode == 2) {
                    uint16_t binary = OP_PEARSON_CORR;
                    group = ray_group_build(graph, keys, 1, &binary, inputs, inputs, NULL, 1);
                } else if (mode == 3) {
                    uint16_t sum = OP_SUM;
                    group = ray_group(graph, keys, 1, &sum, inputs, 1);
                } else group = ray_group(graph, keys, 1, ops, inputs, mode ? 3 : 2);
                agg_route_reset();
                ray_t* out = ray_execute(graph, group);
                TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
                agg_route_stats_t stats = agg_route_stats();
                TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_DENSE], 1);
#if ATOMIC_LLONG_LOCK_FREE == 2
                TEST_ASSERT_EQ_I(stats.dense_strategy, mode || floating || wide_int ? AGG_DENSE_PARTITIONED : AGG_DENSE_SHARED);
#endif
                TEST_ASSERT_TRUE(stats.dense_local_slots <= 2 * 65792);
                TEST_ASSERT_EQ_I(ray_table_nrows(out), 8193);
                ray_t* ko = ray_table_get_col_idx(out, 0);
                for (int64_t row = 0; row < ko->len; row++) {
                    int32_t k = ((int32_t*)ray_data(ko))[row];
                    int index = k == NULL_I32 ? 8192 : k / 8 + 4096;
                    TEST_ASSERT_TRUE(index >= 0 && index <= 8192);
                    if (mode == 3) {
                        ray_t* col = ray_table_get_col_idx(out, 1);
                        TEST_ASSERT_EQ_I(col->type, floating ? RAY_F64 : type == RAY_TIME ? RAY_TIME : RAY_I64);
                        if (floating) TEST_ASSERT_TRUE(((double*)ray_data(col))[row] == sums[index]);
                        else if (type == RAY_TIME) TEST_ASSERT_EQ_I(((int32_t*)ray_data(col))[row], sums[index]);
                        else TEST_ASSERT_EQ_I(((int64_t*)ray_data(col))[row], sums[index]);
                    } else if (mode == 2) {
                        double got = ((double*)ray_data(ray_table_get_col_idx(out, 1)))[row];
                        TEST_ASSERT_TRUE(mn[index] == INT64_MAX ? isnan(got) : fabs(got - 1.0) < 1e-12);
                    } else {
                        for (int a = 0; a < 2; a++) {
                            ray_t* col = ray_table_get_col_idx(out, a + 1);
                            TEST_ASSERT_EQ_I(col->type, type == RAY_F32 ? RAY_F64 : type);
                            if (floating) {
                                double got = ((double*)ray_data(col))[row];
                                TEST_ASSERT_TRUE(mn[index] == INT64_MAX ? isnan(got) : got == (a ? mx[index] : mn[index]));
                                continue;
                            }
                            int64_t got = width == 1 ? ((uint8_t*)ray_data(col))[row]
                                : width == 2 ? ((int16_t*)ray_data(col))[row]
                                : wide_int ? ((int64_t*)ray_data(col))[row] : ((int32_t*)ray_data(col))[row];
                            int64_t want = mn[index] == INT64_MAX ? (width == 2 ? NULL_I16 : wide_int ? NULL_I64 : NULL_I32)
                                : a ? mx[index] : mn[index];
                            TEST_ASSERT_EQ_I(got, want);
                        }
                        if (mode) TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 3)))[row], counts[index]);
                    }
                }
                ray_release(out); ray_graph_free(graph);
            }
        }
        ray_release(tbl);
    }
    PASS();
}

static test_result_t test_dense_symbol_output(void) {
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(8), RAY_OK);
    const int64_t n = 65536;
    int64_t ids[8192];
    for (int k = 0; k < 8192; k++) {
        char name[40];
        int len = snprintf(name, sizeof(name), "dense_key_%d", k);
        ids[k] = ray_sym_intern(name, len);
    }
    const uint8_t widths[] = {RAY_SYM_W32, RAY_SYM_W64};
    for (size_t w = 0; w < sizeof(widths); w++) {
        ray_t* key = ray_sym_vec_new(widths[w], n);
        ray_t* val = ray_vec_new(RAY_TIME, n);
        key->len = val->len = n;
        for (int64_t row = 0; row < n; row++) {
            ray_write_sym(ray_data(key), row, ids[row % 8192], RAY_SYM, key->attrs);
            ((int32_t*)ray_data(val))[row] = row % 8192;
        }
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), key);
        tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), val);
        ray_release(key); ray_release(val);
        for (int mixed = 0; mixed < 2; mixed++) {
            ray_graph_t* graph = ray_graph_new(tbl);
            ray_op_t* keys[] = {ray_scan(graph, "k")};
            ray_op_t* value = ray_scan(graph, "v");
            ray_op_t* inputs[] = {value, value};
            uint16_t ops[] = {OP_MIN, OP_COUNT};
            ray_op_t* group = ray_group(graph, keys, 1, ops, inputs, mixed ? 2 : 1);
            agg_route_reset();
            ray_t* out = ray_execute(graph, group);
            TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
            TEST_ASSERT_EQ_I(agg_route_stats().routes[AGG_ROUTE_V2_DENSE], 1);
#if ATOMIC_LLONG_LOCK_FREE == 2
            TEST_ASSERT_EQ_I(agg_route_stats().dense_strategy, AGG_DENSE_PARTITIONED);
#endif
            TEST_ASSERT_EQ_I(ray_table_nrows(out), 8192);
            ray_t* ko = ray_table_get_col_idx(out, 0);
            ray_t* vo = ray_table_get_col_idx(out, 1);
            TEST_ASSERT_EQ_I(ko->attrs & RAY_SYM_W_MASK, widths[w]);
            TEST_ASSERT_EQ_I(vo->type, RAY_TIME);
            for (int64_t row = 0; row < ko->len; row++) {
                int32_t index = ((int32_t*)ray_data(vo))[row];
                TEST_ASSERT_TRUE(index >= 0 && index < 8192);
                TEST_ASSERT_EQ_I(ray_read_sym(ray_data(ko), row, RAY_SYM, ko->attrs), ids[index]);
                if (mixed) TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 2)))[row], n / 8192);
            }
            ray_release(out); ray_graph_free(graph);
        }
        ray_release(tbl);
    }
    PASS();
}

/* Task input ranges can be disjoint, cross bitmap words, and contain only nulls.
 * Repeating the query also verifies that reused occupancy bits are cleared. */
static test_result_t test_dense_task_local(void) {
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    const int8_t types[] = { RAY_I16, RAY_I32, RAY_I64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
    const int64_t n = 400000;
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        ray_t* key = ray_vec_new(types[t], n);
        ray_t* val = ray_vec_new(RAY_TIME, n);
        key->len = val->len = n;
        key->attrs |= RAY_ATTR_HAS_NULLS;
        val->attrs |= RAY_ATTR_HAS_NULLS;
        int64_t counts[513] = {0};
        for (int64_t i = 0; i < n; i++) {
            int64_t k = i / 100000 * 128 + i % 64 - 257;
            bool null = i < 100000 || i % 10007 == 0;
            counts[null ? 512 : k + 257]++;
            if (types[t] == RAY_I16) ((int16_t*)ray_data(key))[i] = null ? NULL_I16 : k;
            else if (types[t] == RAY_I64 || types[t] == RAY_TIMESTAMP)
                ((int64_t*)ray_data(key))[i] = null ? NULL_I64 : k;
            else ((int32_t*)ray_data(key))[i] = null ? NULL_I32 : k;
            ((int32_t*)ray_data(val))[i] = null ? -123 : k == -100 ? NULL_I32 : k * 3;
        }
        ray_t* tbl = ray_table_new(2);
        tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), key);
        tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), val);
        ray_release(key); ray_release(val);
        for (int repeat = 0; repeat < 3; repeat++) {
            ray_graph_t* graph = ray_graph_new(tbl);
            ray_op_t* keys[] = {ray_scan(graph, "k")};
            ray_op_t* values[] = {ray_scan(graph, "v"), ray_scan(graph, "v"), ray_scan(graph, "v")};
            uint16_t ops[] = {OP_MIN, OP_MAX, OP_COUNT};
            ray_op_t* group = ray_group(graph, keys, 1, ops, values, 3);
            agg_route_reset();
            int64_t watermark = ray_heap_anon_watermark();
            if (repeat == 2) {
                /* Allow four task slabs and the final state, but not sixteen.
                 * A small spill budget must constrain extra scheduling tasks. */
                size_t block = 2 * agg_resolve(OP_MIN, RAY_TIME)->state_size
                    + agg_resolve(OP_COUNT, RAY_TIME)->state_size;
                int64_t slab = 321 * (block + sizeof(int64_t) + 1);
                ray_heap_set_anon_watermark(4 * (n * (int64_t)sizeof(uint32_t) + 5 * slab));
            }
            ray_t* out = ray_execute(graph, group);
            ray_heap_set_anon_watermark(watermark);
            TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
            agg_route_stats_t stats = agg_route_stats();
            TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_DENSE], 1);
            TEST_ASSERT_TRUE(stats.dense_tasks >= 4 && stats.dense_tasks <= 16);
            if (repeat == 2) TEST_ASSERT_EQ_I(stats.dense_tasks, 4);
            TEST_ASSERT_EQ_I(stats.dense_strategy, AGG_DENSE_TASK_LOCAL);
            TEST_ASSERT_EQ_I(stats.dense_local_slots, stats.dense_tasks * 321);
            TEST_ASSERT_EQ_I(ray_table_nrows(out), 193);
            ray_t* ko = ray_table_get_col_idx(out, 0);
            ray_t* mn = ray_table_get_col_idx(out, 1);
            ray_t* mx = ray_table_get_col_idx(out, 2);
            ray_t* count = ray_table_get_col_idx(out, 3);
            TEST_ASSERT_EQ_I(ko->type, types[t]);
            TEST_ASSERT_EQ_I(mn->type, RAY_TIME); TEST_ASSERT_EQ_I(mx->type, RAY_TIME);
            for (int64_t r = 0; r < ko->len; r++) {
                int64_t k = types[t] == RAY_I16 ? ((int16_t*)ray_data(ko))[r]
                    : types[t] == RAY_I64 || types[t] == RAY_TIMESTAMP ? ((int64_t*)ray_data(ko))[r]
                    : ((int32_t*)ray_data(ko))[r];
                int64_t null = types[t] == RAY_I16 ? NULL_I16
                    : types[t] == RAY_I64 || types[t] == RAY_TIMESTAMP ? NULL_I64 : NULL_I32;
                int32_t expected = k == null ? -123 : k == -100 ? NULL_I32 : k * 3;
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(mn))[r], expected);
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(mx))[r], expected);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(count))[r], counts[k == null ? 512 : k + 257]);
            }
            ray_release(out); ray_graph_free(graph);
        }
        ray_release(tbl);
    }
    PASS();
}

static test_result_t test_rank_widths_nulls_and_slices(void) {
    const int8_t types[] = { RAY_BOOL, RAY_F32 };
    for (int t = 0; t < 2; t++) {
        ray_t* empty = ray_vec_new(types[t], 0);
        int64_t zero = 0;
        ray_t* empty_group = ray_median_per_group_buf(empty, NULL, &zero, &zero, 1);
        TEST_ASSERT_NOT_NULL(empty_group);
        TEST_ASSERT_EQ_I(empty_group->type, RAY_F64);
        TEST_ASSERT_TRUE(ray_vec_is_null(empty_group, 0));
        ray_release(empty_group); ray_release(empty);
        for (int large = 0; large < 2; large++) {
            int64_t group_size = large ? 513 : 3;
            int64_t n = 16 * group_size;
            ray_t* src = ray_vec_new(types[t], n + 7); src->len = n + 7;
            for (int64_t i = 0; i < src->len; i++) {
                int64_t g = i < 7 ? 99 : (i - 7) / group_size;
                if (t) ((float*)ray_data(src))[i] = g == 0 ? NULL_F32 : (float)g + 0.25f;
                else ((uint8_t*)ray_data(src))[i] = g % 2;
            }
            if (t) src->attrs |= RAY_ATTR_HAS_NULLS;
            ray_t* slice = ray_vec_slice(src, 7, n);
            if (t) {
                /* Views may inherit null metadata from their parent. */
                slice->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
                int64_t winners[] = {0, n - 1};
                ray_t* gathered = ray_group_gather(slice, winners, 2);
                TEST_ASSERT_FMT(gathered && !RAY_IS_ERR(gathered), "slice gather failed");
                TEST_ASSERT_TRUE(gathered->attrs & RAY_ATTR_HAS_NULLS);
                TEST_ASSERT_TRUE(ray_vec_is_null(gathered, 0));
                TEST_ASSERT_EQ_F(((float*)ray_data(gathered))[1], 15.25, 1e-12);
                ray_release(gathered);
            }
            int64_t offsets[16], counts[16];
            ray_t* indices = ray_vec_new(RAY_I64, n); indices->len = n;
            for (int64_t g = 0; g < 16; g++) {
                offsets[g] = g * group_size; counts[g] = group_size;
                for (int64_t j = 0; j < group_size; j++)
                    ((int64_t*)ray_data(indices))[offsets[g]+j] = offsets[g]+group_size-1-j;
            }
            for (int q = 0; q < 2; q++) {
                ray_t* out = q
                    ? ray_quantile_per_group_buf(slice, ray_data(indices), offsets, counts, 16, 0.25)
                    : ray_median_per_group_buf(slice, ray_data(indices), offsets, counts, 16);
                TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
                TEST_ASSERT_EQ_I(out->type, RAY_F64);
                for (int64_t g = 0; g < 16; g++) {
                    if (t && g == 0) {
                        TEST_ASSERT_TRUE(ray_vec_is_null(out, g));
                        TEST_ASSERT_TRUE(out->attrs & RAY_ATTR_HAS_NULLS);
                    } else TEST_ASSERT_EQ_F(((double*)ray_data(out))[g], t ? g+0.25 : g%2, 1e-12);
                }
                ray_release(out);
            }
            ray_release(indices); ray_release(slice); ray_release(src);
        }
    }
    PASS();
}

static test_result_t test_pairwise_numeric_contracts(void) {
    const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64 };
    const char* names[] = { "pearson_corr", "cov", "scov", "wsum", "wavg" };
    for (size_t x = 0; x < sizeof(types)/sizeof(types[0]); x++) {
        for (size_t y = 0; y < sizeof(types)/sizeof(types[0]); y++) {
            ray_t* vx = contract_fixture(types[x]);
            ray_t* vy = contract_fixture(types[y]);
            ray_env_set(ray_sym_intern("x", 1), vx);
            ray_env_set(ray_sym_intern("y", 1), vy);
            ray_release(vx); ray_release(vy);
            ray_t* init = ray_eval_str("(set t (table [k x y] (list [0 0 1 1] x y)))");
            TEST_ASSERT_FALSE(RAY_IS_ERR(init)); ray_release(init);
            for (size_t a = 0; a < sizeof(names)/sizeof(names[0]); a++) {
                char source[256];
                snprintf(source, sizeof(source),
                         "(at (select {from:t by:k s:(%s x y) asc:k}) 's)", names[a]);
                ray_t* got = ray_eval_str(source);
                TEST_ASSERT_FMT(got && !RAY_IS_ERR(got), "%s(%s,%s) grouped failed",
                                names[a], ray_type_name(types[x]), ray_type_name(types[y]));
                TEST_ASSERT_EQ_I(got->type, RAY_F64);
                TEST_ASSERT_EQ_I(got->len, 2);
                for (int g = 0; g < 2; g++) {
                    snprintf(source, sizeof(source), "(%s (at x [%d %d]) (at y [%d %d]))",
                             names[a], 2*g, 2*g+1, 2*g, 2*g+1);
                    ray_t* want = ray_eval_str(source);
                    TEST_ASSERT_FMT(want && !RAY_IS_ERR(want), "%s scalar failed", source);
                    TEST_ASSERT_EQ_I(want->type, -RAY_F64);
                    double actual = ((double*)ray_data(got))[g];
                    bool equal = (isnan(actual) && isnan(want->f64)) || fabs(actual - want->f64) <= 1e-12;
                    ray_release(want);
                    TEST_ASSERT_FMT(equal, "%s(%s,%s) group %d differs from scalar slice",
                                    names[a], ray_type_name(types[x]), ray_type_name(types[y]), g);
                }
                ray_release(got);
            }
        }
    }
    PASS();
}

static test_result_t test_native_binary_output(void) {
    const uint16_t ops[] = { OP_PEARSON_CORR, OP_COV, OP_SCOV, OP_WSUM, OP_WAVG };
    const double expected[][4] = {
        {1.0, NULL_F64, NULL_F64, NULL_F64},
        {0.5, 0.0, NULL_F64, NULL_F64},
        {1.0, 0.0, NULL_F64, NULL_F64},
        {10.0, 0.0, 0.0, 0.0},
        {10.0/3.0, NULL_F64, NULL_F64, NULL_F64},
    };
    const double xs[] = {1, 2, 0, 0, NULL_F64, NULL_F64};
    const double ys[] = {2, 4, 3, 5, 7, NULL_F64};
    const uint32_t gids[] = {0, 0, 1, 1, 2, 2};
    ray_t* x = ray_vec_new(RAY_F64, 6); x->len = 6;
    ray_t* y = ray_vec_new(RAY_F64, 6); y->len = 6;
    memcpy(ray_data(x), xs, sizeof(xs)); memcpy(ray_data(y), ys, sizeof(ys));
    x->attrs |= RAY_ATTR_HAS_NULLS; y->attrs |= RAY_ATTR_HAS_NULLS;
    for (size_t a = 0; a < sizeof(ops)/sizeof(ops[0]); a++) {
        ray_t* out = agg_run_one_bin(agg_resolve(ops[a], RAY_F64), x, y, gids, 6, 4, 0);
        TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
        TEST_ASSERT_EQ_I(out->type, RAY_F64); TEST_ASSERT_EQ_I(out->len, 4);
        for (int g = 0; g < 4; g++) {
            if (isnan(expected[a][g])) {
                TEST_ASSERT_TRUE(ray_vec_is_null(out, g));
                TEST_ASSERT_TRUE(out->attrs & RAY_ATTR_HAS_NULLS);
            } else TEST_ASSERT_EQ_F(((double*)ray_data(out))[g], expected[a][g], 1e-12);
        }
        ray_release(out);
    }
    ray_release(x); ray_release(y);
    PASS();
}

static test_result_t test_nullable_differential(void) {
    const char* types[] = {"I16", "I32", "I64", "F32", "F64", "DATE", "TIME", "TIMESTAMP", "SYM", "STR"};
    const char* operations[] = {"sum v", "min v", "max v", "avg v", "var v", "var_pop v", "stddev v", "stddev_pop v", "prod v", "first v", "last v", "med v", "mode v", "quantile v 0.25", "top v 2", "bot v 2"};
    for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++) {
        char source[512];
        if (t < 8) {
            const int8_t ts[] = { RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP };
            ray_t* v = ray_vec_new(ts[t], 6); v->len = 6;
            const int vals[] = {0, 7, 0, 0, 9, 3};
            for (int i = 0; i < 6; i++) {
                if (ts[t] == RAY_F32) ((float*)ray_data(v))[i] = vals[i];
                else if (ts[t] == RAY_F64) ((double*)ray_data(v))[i] = vals[i];
                else if (ts[t] == RAY_I16) ((int16_t*)ray_data(v))[i] = vals[i];
                else if (ts[t] == RAY_I64 || ts[t] == RAY_TIMESTAMP) ((int64_t*)ray_data(v))[i] = vals[i];
                else ((int32_t*)ray_data(v))[i] = vals[i];
            }
            ray_vec_set_null(v, 0, true); ray_vec_set_null(v, 2, true); ray_vec_set_null(v, 3, true);
            ray_env_set(ray_sym_intern("v", 1), v); ray_release(v);
            snprintf(source, sizeof(source), "(set t (table [k v] (list [0 0 1 1 2 2] v)))");
        } else snprintf(source, sizeof(source), "(set t (table [k v] (list [0 0 1 1 2 2] (as '%s [\"\" \"b\" \"\" \"\" \"z\" \"a\"]))))", types[t]);
        ray_t* init = ray_eval_str(source);
        TEST_ASSERT_FMT(init && !RAY_IS_ERR(init), "fixture %s", types[t]); ray_release(init);
        for (size_t a = 0; a < sizeof(operations)/sizeof(operations[0]); a++) {
            snprintf(source, sizeof(source), "(at (select {from:t by:k asc:k s:(%s)}) 's)", operations[a]);
            ray_agg_engine_v2 = false; ray_t* old = ray_eval_str(source);
            ray_agg_engine_v2 = true; ray_t* got = ray_eval_str(source);
            bool oe = old && RAY_IS_ERR(old), ge = got && RAY_IS_ERR(got);
            TEST_ASSERT_FMT(oe == ge, "%s/%s error parity", types[t], operations[a]);
            if (oe) { ray_error_free(old); ray_error_free(got); continue; }
            TEST_ASSERT_FMT(old && got && old->type == got->type && old->len == got->len,
                            "%s/%s output shape", types[t], operations[a]);
            ray_t* os = ray_fmt(old, 0); ray_t* gs = ray_fmt(got, 0);
            const char* of = ray_str_ptr(os); const char* gf = ray_str_ptr(gs);
            bool same = of && gf && strcmp(of, gf) == 0;
            TEST_ASSERT_FMT(same, "%s/%s: old %s, new %s", types[t], operations[a], of, gf);
            ray_release(os); ray_release(gs); ray_release(old); ray_release(got);
        }
    }
    PASS();
}

static test_result_t test_wide_key_routes(void) {
    const int8_t types[] = {RAY_F32, RAY_F64, RAY_GUID, RAY_STR, RAY_LIST};
    for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++) {
        ray_t* k;
        if (types[t] == RAY_STR) k = ray_eval_str("[\"long pooled duplicate value\" \"long pooled duplicate value\" \"a\" \"a\" \"\" \"\"]");
        else if (types[t] == RAY_LIST) k = ray_eval_str("(list ['a 'b] ['a 'b] [\"long pooled string value\" \"x\"] [\"long pooled string value\" \"x\"] [1 2] [1 2])");
        else {
            k = ray_vec_new(types[t], 6); k->len = 6;
            if (types[t] == RAY_GUID) {
                memset(ray_data(k), 0, 6 * 16);
                for (int i = 0; i < 4; i++) ((uint8_t*)ray_data(k))[i * 16] = (uint8_t)(i / 2 + 1);
            } else for (int i = 0; i < 6; i++) {
                double v = i < 2 ? (i ? -0.0 : 0.0) : i < 4 ? 1 : NAN;
                if (types[t] == RAY_F32) ((float*)ray_data(k))[i] = (float)v;
                else ((double*)ray_data(k))[i] = v;
            }
            ray_vec_set_null(k, 4, true);
        }
        TEST_ASSERT_NOT_NULL(k); TEST_ASSERT_FALSE(RAY_IS_ERR(k));
        if (types[t] == RAY_F32 || types[t] == RAY_F64) {
            k->attrs &= (uint8_t)~RAY_ATTR_HAS_NULLS;
            if (types[t] == RAY_F32) {
                uint32_t bits[] = { UINT32_C(0x7fc00001), UINT32_C(0x7fc00002) };
                memcpy((float*)ray_data(k) + 4, bits, sizeof(bits));
            } else {
                uint64_t bits[] = { UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff8000000000002) };
                memcpy((double*)ray_data(k) + 4, bits, sizeof(bits));
            }
            agg_groups_t groups = {0}; ray_t* one_key[] = {k};
            TEST_ASSERT_EQ_I(agg_group_keys(one_key, 1, 6, &groups), 0);
            TEST_ASSERT_EQ_I(groups.ngroups, 4); /* unflagged payloads stay distinct */
            agg_groups_free(&groups);
            k->attrs |= RAY_ATTR_HAS_NULLS; /* flagged NaNs merge as null below */
        }
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), k); ray_release(k);
        ray_graph_t* graph = ray_graph_new(tbl);
        ray_op_t* keys[] = {ray_scan(graph, "k")};
        uint16_t op = OP_COUNT;
        ray_op_t* group = ray_group(graph, keys, 1, &op, keys, 1);
        agg_route_reset();
        ray_t* out = ray_execute(graph, group);
        TEST_ASSERT_FMT(out && !RAY_IS_ERR(out), "key type %s failed", ray_type_name(types[t]));
        TEST_ASSERT_EQ_I(agg_route_stats().routes[AGG_ROUTE_V2_INDEXED], 1);
        TEST_ASSERT_EQ_I(ray_table_nrows(out), 3);
        TEST_ASSERT_EQ_I(ray_table_get_col_idx(out, 0)->type, types[t]);
        ray_t* counts = ray_table_get_col_idx(out, 1);
        for (int i = 0; i < 3; i++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(counts))[i], 2);
        ray_release(out); ray_graph_free(graph); ray_release(tbl);
    }
    agg_route_reset();
    ray_t* large = ray_eval_str("(select {from:(table [k] (list (take [\"a\" \"b\"] 65536))) by:k n:(count k)})");
    TEST_ASSERT_NOT_NULL(large); TEST_ASSERT_FALSE(RAY_IS_ERR(large));
    TEST_ASSERT_EQ_I(agg_route_stats().last_v2_reason, AGG_V2_ADMITTED);
    TEST_ASSERT_EQ_I(agg_route_stats().routes[AGG_ROUTE_V2_INDEXED], 1);
    TEST_ASSERT_EQ_I(ray_table_nrows(large), 2);
    ray_release(large);
    agg_route_reset();
    large = ray_eval_str("(select {from:(table [k j] (list (take [\"a\" \"b\"] 65536) (map (fn [x] (list (% x 2))) (til 65536)))) by:[k j] n:(count k)})");
    TEST_ASSERT_NOT_NULL(large); TEST_ASSERT_FALSE(RAY_IS_ERR(large));
    TEST_ASSERT_EQ_I(agg_route_stats().routes[AGG_ROUTE_V2_INDEXED], 1);
    TEST_ASSERT_EQ_I(ray_table_nrows(large), 2);
    ray_release(large);
    agg_route_reset();
    ray_t* out = ray_eval_str("(select {from:(table [k a b] (list [0 0 1 1] (as 'TIME [1 2 3 4]) (as 'TIME [5 7 10 12]))) by:k s:(sum (- b a))})");
    TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(agg_route_stats().routes[AGG_ROUTE_V2_SERIAL_DENSE], 1);
    TEST_ASSERT_EQ_I(ray_table_get_col_idx(out, 1)->type, RAY_TIME);
    ray_release(out);
    PASS();
}

static test_result_t test_fused_typed_comparisons(void) {
    const int8_t types[] = {RAY_I64, RAY_F32, RAY_F64, RAY_TIME, RAY_GUID, RAY_STR};
    ray_op_t* (*ctors[])(ray_graph_t*, ray_op_t*, ray_op_t*) = {ray_eq, ray_ne, ray_lt, ray_le, ray_gt, ray_ge};
    ray_t* (*fns[])(ray_t*, ray_t*) = {ray_eq_fn, ray_neq_fn, ray_lt_fn, ray_lte_fn, ray_gt_fn, ray_gte_fn};
    for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++) {
        ray_t* col = contract_fixture(types[t]);
        ray_vec_set_null(col, 0, true);
        ray_t* tbl = ray_table_new(1);
        tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), col);
        for (int c = 0; c < 2; c++) {
            int allocated = 0;
            ray_t* constant = collection_elem(col, c, &allocated);
            for (int op = 0; op < 6; op++) {
                ray_graph_t* graph = ray_graph_new(tbl);
                ray_op_t* lhs = ray_scan(graph, "v");
                uint32_t id = lhs->id;
                ray_op_t* rhs = ray_const_atom(graph, constant);
                ray_op_t* predicate = ctors[op](graph, &graph->nodes[id], rhs);
                fp_pred_t compiled = {0};
                TEST_ASSERT_FMT(fp_compile_pred(graph, predicate, tbl, &compiled) == 0,
                                "fused %s comparison %d declined", ray_type_name(types[t]), op);
                uint8_t bits[4]; fp_eval_pred(&compiled, 0, 4, bits);
                for (int r = 0; r < 4; r++) {
                    int al = 0; ray_t* cell = collection_elem(col, r, &al);
                    ray_t* expected = fns[op](cell, constant);
                    TEST_ASSERT_FMT(expected && !RAY_IS_ERR(expected), "comparison oracle failed");
                    TEST_ASSERT_FMT(bits[r] == expected->b8, "%s op %d row %d null constant %d", ray_type_name(types[t]), op, r, c == 0);
                    ray_release(expected); if (al) ray_release(cell);
                }
                fp_pred_cleanup(&compiled); ray_graph_free(graph);
            }
            if (allocated) ray_release(constant);
        }
        ray_release(col); ray_release(tbl);
    }
    PASS();
}

static test_result_t test_count_distinct_typed_routes(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(2), RAY_OK);
    const int8_t types[] = { RAY_BOOL, RAY_U8, RAY_I32, RAY_TIME, RAY_TIMESTAMP, RAY_SYM, RAY_F32, RAY_F64 };
    const int64_t n = 262144;
    for (size_t t = 0; t < sizeof(types)/sizeof(types[0]); t++) {
        int8_t type = types[t];
        ray_t* k = type == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W64, n) : ray_vec_new(type, n);
        ray_t* v = ray_vec_new(RAY_F32, n); k->len = v->len = n;
        int64_t sym = ray_sym_intern("typed_cdf", 9);
        for (int64_t i = 0; i < n; i++) {
            int64_t key = i % 2;
            if (type == RAY_BOOL || type == RAY_U8) ((uint8_t*)ray_data(k))[i] = (uint8_t)key;
            else if (type == RAY_I32 || type == RAY_TIME) ((int32_t*)ray_data(k))[i] = key ? 1 : NULL_I32;
            else if (type == RAY_TIMESTAMP) ((int64_t*)ray_data(k))[i] = key ? 1 : NULL_I64;
            else if (type == RAY_SYM) ((int64_t*)ray_data(k))[i] = key ? sym : 0;
            else if (type == RAY_F32) ((float*)ray_data(k))[i] = key ? 1 : NAN;
            else ((double*)ray_data(k))[i] = key ? 1 : NAN;
            int value = (int)((i / 2) % 3);
            ((float*)ray_data(v))[i] = value ? (float)value : NAN;
        }
        if (type != RAY_BOOL && type != RAY_U8) k->attrs |= RAY_ATTR_HAS_NULLS;
        v->attrs |= RAY_ATTR_HAS_NULLS;
        ray_t* result = ray_cd_fused(k, v, n);
        TEST_ASSERT_FMT(result && !RAY_IS_ERR(result), "count-distinct route %s declined", ray_type_name(type));
        TEST_ASSERT_EQ_I(ray_table_nrows(result), 2);
        ray_t* counts = ray_table_get_col_idx(result, 1);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(counts))[0], 3);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(counts))[1], 3);
        ray_release(result); ray_release(k); ray_release(v);
    }
    /* Large first-seen ordering and repeated-pair skew. The bitmap oracle
     * counts values independently and retains the earliest original row. */
    const int64_t ng = 16384;
    for (int skew = 0; skew < 2; skew++) {
        ray_t* k = ray_vec_new(RAY_I64, n);
        ray_t* v = ray_vec_new(RAY_I64, n);
        TEST_ASSERT_NOT_NULL(k); TEST_ASSERT_NOT_NULL(v);
        k->len = v->len = n;
        uint8_t seen[16385] = {0};
        int64_t first[16385];
        for (int64_t g = 0; g <= ng; g++) first[g] = -1;
        for (int64_t i = 0; i < n; i++) {
            bool hot = skew && i % 3 == 0;
            int64_t group = hot ? ng : (i * 17) % ng;
            int value = hot ? (int)((i / 3) % 4) : (int)((i / ng) % 3);
            ((int64_t*)ray_data(k))[i] = hot ? -1 : group;
            ((int64_t*)ray_data(v))[i] = value;
            seen[group] |= (uint8_t)(1u << value);
            if (first[group] < 0) first[group] = i;
        }
        ray_t* result = ray_cd_fused(k, v, n);
        TEST_ASSERT_FMT(result && !RAY_IS_ERR(result), "large count-distinct declined");
        TEST_ASSERT_EQ_I(ray_table_nrows(result), ng + skew);
        int64_t* keys = ray_data(ray_table_get_col_idx(result, 0));
        int64_t* counts = ray_data(ray_table_get_col_idx(result, 1));
        int64_t* firsts = ray_data(ray_table_get_col_idx(result, 2));
        int64_t previous = -1;
        for (int64_t i = 0; i < ng + skew; i++) {
            int64_t group = keys[i] == -1 ? ng : keys[i];
            TEST_ASSERT_TRUE(group >= 0 && group <= ng);
            TEST_ASSERT_EQ_I(counts[i], __builtin_popcount(seen[group]));
            TEST_ASSERT_EQ_I(firsts[i], first[group]);
            TEST_ASSERT_TRUE(firsts[i] > previous); previous = firsts[i];
        }
        ray_release(result); ray_release(k); ray_release(v);
    }
    PASS();
}

/* Independent membership/order/reduction oracle for the parallel shared
 * directory and row slices. A dominant null key crosses many reduction tasks. */
static test_result_t test_wide_count_distinct(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    const char* vocabularies[] = {
        "[\"pooled long alpha value\" \"beta\" \"pooled long alpha value\" \"\"]",
        "(as 'GUID (list \"00000000-0000-0000-0000-000000000001\" \"00000000-0000-0000-0000-000000000002\" \"00000000-0000-0000-0000-000000000001\" \"\"))",
        "(list [1 2] [3 4] [1 2] (list))",
    };
    const int8_t types[] = {RAY_STR, RAY_GUID, RAY_LIST};
    ray_t* setup = ray_eval_str("(set i (til 262144))");
    TEST_ASSERT_TRUE(setup && !RAY_IS_ERR(setup)); ray_release(setup);
    for (int kind = 0; kind < 3; kind++) {
        char script[1024];
        snprintf(script, sizeof(script),
            "(set t (table [i k v] (list i (as 'I32 (%% i 65536)) (at %s (as 'I64 (/ i 65536))))))",
            vocabularies[kind]);
        setup = ray_eval_str(script);
        TEST_ASSERT_TRUE(setup && !RAY_IS_ERR(setup));
        TEST_ASSERT_EQ_I(ray_table_get_col_idx(setup, 2)->type, types[kind]);
        ray_t* source = ray_table_get_col_idx(setup, 2); ray_retain(source);
        ray_release(setup);
        ray_t* grouped = ray_group_indices_fn(source);
        TEST_ASSERT_TRUE(grouped && !RAY_IS_ERR(grouped));
        ray_t* indices = ray_dict_vals(grouped);
        TEST_ASSERT_EQ_I(indices->len, 3);
        for (int g = 0; g < 3; g++) {
            ray_t* rows = ray_list_get(indices, g);
            TEST_ASSERT_EQ_I(rows->type, RAY_I64);
            TEST_ASSERT_EQ_I(rows->len, g ? 65536 : 131072);
            for (int64_t i = 0; i < rows->len; i++) {
                int64_t expected = g == 0 ? (i < 65536 ? i : i + 65536)
                    : g == 1 ? i + 65536 : i + 196608;
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(rows))[i], expected);
            }
        }
        ray_release(grouped); ray_release(source);
        for (int shape = 0; shape < 4; shape++) {
            int selected = shape & 1;
            snprintf(script, sizeof(script), "(select {from:t by:k s:(count (distinct v)) %s %s})",
                selected ? "where:(< i 196608)" : "", shape >= 2 ? "total:(sum i)" : "");
            ray_t* out = ray_eval_str(script);
            TEST_ASSERT_FMT(out && !RAY_IS_ERR(out), "wide count distinct failed, type %d", types[kind]);
            TEST_ASSERT_EQ_I(ray_table_nrows(out), 65536);
            ray_t* counts = ray_table_get_col(out, ray_sym_intern("s", 1));
            TEST_ASSERT_NOT_NULL(counts); TEST_ASSERT_EQ_I(counts->type, RAY_I64);
            for (int64_t g = 0; g < counts->len; g++)
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(counts))[g], selected ? 2 : 3);
            ray_release(out);
        }
        ray_t* out = ray_eval_str("(select {from:t by:v s:(count (distinct k)) total:(sum i)})");
        TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
        TEST_ASSERT_EQ_I(ray_table_nrows(out), 3);
        ray_t* counts = ray_table_get_col(out, ray_sym_intern("s", 1));
        TEST_ASSERT_NOT_NULL(counts);
        for (int64_t g = 0; g < counts->len; g++)
            TEST_ASSERT_EQ_I(((int64_t*)ray_data(counts))[g], 65536);
        ray_release(out);
        out = ray_eval_str("(select {from:t by:v where:(< i 0)})");
        TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
        TEST_ASSERT_EQ_I(ray_table_nrows(out), 0);
        TEST_ASSERT_EQ_I(ray_table_ncols(out), 3);
        TEST_ASSERT_EQ_I(ray_table_get_col(out, ray_sym_intern("v", 1))->type, types[kind]);
        ray_release(out);
        out = ray_eval_str("(select {from:t by:v s:(count (distinct k)) total:(sum i) m:(med i) where:(< i 0)})");
        TEST_ASSERT_TRUE(out && !RAY_IS_ERR(out));
        TEST_ASSERT_EQ_I(ray_table_nrows(out), 0);
        TEST_ASSERT_EQ_I(ray_table_ncols(out), 4);
        TEST_ASSERT_EQ_I(ray_table_get_col(out, ray_sym_intern("v", 1))->type, types[kind]);
        TEST_ASSERT_EQ_I(ray_table_get_col(out, ray_sym_intern("s", 1))->type, RAY_I64);
        TEST_ASSERT_EQ_I(ray_table_get_col(out, ray_sym_intern("total", 5))->type, RAY_I64);
        TEST_ASSERT_EQ_I(ray_table_get_col(out, ray_sym_intern("m", 1))->type, RAY_F64);
        ray_release(out);

    }
    PASS();
}

static test_result_t test_indexed_parallel_layout(void) {
    const int64_t n = 131075;
    const int64_t group_counts[] = {4096, 17, 7, 17};
    for (int shape = 0; shape < 4; shape++) {
        ray_pool_destroy();
        if (shape < 3) TEST_ASSERT_EQ_I(ray_pool_init_total(shape ? 3 : 4), RAY_OK);
        const int64_t ng = group_counts[shape];
        int64_t symbols[4096];
        for (int64_t k = 0; k < ng; k++) {
            char name[32]; int len = snprintf(name, sizeof(name), "nested-key-%lld", (long long)k);
            symbols[k] = ray_sym_intern(name, len);
        }
        const int8_t types[] = {RAY_I32, RAY_I64, RAY_F64, RAY_GUID, RAY_STR, RAY_LIST};
        for (size_t ti = 0; ti < sizeof(types) / sizeof(types[0]); ti++) {
            int8_t type = types[ti];
            ray_t* key = type == RAY_LIST ? ray_list_new(n) : ray_vec_new(type, n);
            ray_t* val = ray_vec_new(RAY_I64, n);
            TEST_ASSERT_NOT_NULL(key); TEST_ASSERT_NOT_NULL(val);
            key->len = val->len = n;
            key->attrs |= RAY_ATTR_HAS_NULLS; val->attrs |= RAY_ATTR_HAS_NULLS;
            int64_t count[4096] = {0}, valid[4096] = {0}, sum[4096] = {0};
            int64_t first[4096], last[4096], first_row[4096];
            uint32_t* hist = ray_calloc_raw((size_t)ng * 101 * sizeof(uint32_t));
            TEST_ASSERT_NOT_NULL(hist);
            for (int64_t k = 0; k < ng; k++) first[k] = last[k] = first_row[k] = -1;
            for (int64_t r = 0; r < n; r++) {
                int64_t k = r % 3 == 0 ? 0 : r % ng;
                int64_t v = r % 101;
                bool null = r % 97 == 0 || k == 17;
                if (type == RAY_I32) ((int32_t*)ray_data(key))[r] = k ? k : NULL_I32;
                else if (type == RAY_I64) ((int64_t*)ray_data(key))[r] = k ? k * INT64_C(1000000007) : NULL_I64;
                else if (type == RAY_F64) ((double*)ray_data(key))[r] = k ? (double)k : NAN;
                else if (type == RAY_GUID) { memset((char*)ray_data(key) + r * 16, 0, 16); memcpy((char*)ray_data(key) + r * 16, &k, sizeof(k)); }
                else if (type == RAY_LIST) {
                    ray_t* item = NULL;
                    if (k) {
                        item = ray_sym_vec_new(r % 2 ? RAY_SYM_W32 : RAY_SYM_W64, 1);
                        TEST_ASSERT_NOT_NULL(item); item->len = 1;
                        ray_write_sym(ray_data(item), 0, symbols[k], RAY_SYM, item->attrs);
                    }
                    ((ray_t**)ray_data(key))[r] = item;
                }
                else {
                    char text[32]; int len = k ? snprintf(text, sizeof(text), "key%lld", (long long)k) : 0;
                    key = ray_str_vec_set(key, r, text, len);
                    TEST_ASSERT_NOT_NULL(key); TEST_ASSERT_FALSE(RAY_IS_ERR(key));
                }
                ((int64_t*)ray_data(val))[r] = null ? NULL_I64 : v;
                if (first_row[k] < 0) first_row[k] = r;
                count[k]++;
                if (!null) {
                    if (first[k] < 0) first[k] = v;
                    last[k] = v; valid[k]++; sum[k] += v; hist[k * 101 + v]++;
                }
            }
            ray_t* tbl = ray_table_new(2);
            tbl = ray_table_add_col(tbl, ray_sym_intern("k", 1), key);
            tbl = ray_table_add_col(tbl, ray_sym_intern("v", 1), val);
            ray_release(key); ray_release(val);
            for (int repeat = 0; repeat < 2; repeat++) {
                ray_graph_t* graph = ray_graph_new(tbl);
                ray_op_t* keys[] = {ray_scan(graph, "k")};
                ray_op_t* v = ray_scan(graph, "v");
                ray_op_t* inputs[] = {v, v, v, v, v};
                uint16_t ops[] = {OP_SUM, OP_FIRST, OP_LAST, OP_MEDIAN, OP_COUNT};
                ray_op_t* group = ray_group(graph, keys, 1, ops, inputs, 5);
                agg_route_reset();
                ray_t* out = ray_execute(graph, group);
                TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
                TEST_ASSERT_EQ_I(agg_route_stats().routes[AGG_ROUTE_V2_INDEXED], 1);
                TEST_ASSERT_EQ_I(ray_table_nrows(out), ng);
                ray_t* ko = ray_table_get_col_idx(out, 0);
                TEST_ASSERT_EQ_I(ko->type, type);
                int64_t previous = -1;
                for (int64_t r = 0; r < ng; r++) {
                    int64_t k;
                    if (type == RAY_I32) { k = ((int32_t*)ray_data(ko))[r]; if (k == NULL_I32) k = 0; }
                    else if (type == RAY_I64) { k = ((int64_t*)ray_data(ko))[r]; k = k == NULL_I64 ? 0 : k / INT64_C(1000000007); }
                    else if (type == RAY_F64) { double f = ((double*)ray_data(ko))[r]; k = isnan(f) ? 0 : (int64_t)f; }
                    else if (type == RAY_GUID) memcpy(&k, (char*)ray_data(ko) + r * 16, sizeof(k));
                    else if (type == RAY_LIST) {
                        ray_t* item = ray_list_get(ko, r);
                        k = 0;
                        if (item) {
                            int64_t id = ray_read_sym(ray_data(item), 0, RAY_SYM, item->attrs);
                            while (k < ng && symbols[k] != id) k++;
                        }
                    }
                    else {
                        size_t len = 0; const char* text = ray_str_vec_get(ko, r, &len);
                        k = 0; for (size_t i = 3; i < len; i++) k = k * 10 + text[i] - '0';
                    }
                    TEST_ASSERT_TRUE(k >= 0 && k < ng);
                    TEST_ASSERT_TRUE(first_row[k] > previous); previous = first_row[k];
                    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 1)))[r], sum[k]);
                    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 2)))[r], first[k] < 0 ? NULL_I64 : first[k]);
                    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 3)))[r], last[k] < 0 ? NULL_I64 : last[k]);
                    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 5)))[r], count[k]);
                    double median = ((double*)ray_data(ray_table_get_col_idx(out, 4)))[r];
                    if (!valid[k]) TEST_ASSERT_TRUE(isnan(median));
                    else {
                        int64_t cumulative = 0, a = -1, b = -1;
                        for (int value = 0; value <= 100; value++) {
                            cumulative += hist[k * 101 + value];
                            if (a < 0 && cumulative > (valid[k] - 1) / 2) a = value;
                            if (b < 0 && cumulative > valid[k] / 2) b = value;
                        }
                        TEST_ASSERT_TRUE(median == (a + b) / 2.0);
                    }
                }
                ray_release(out); ray_graph_free(graph);
            }
            ray_free_raw(hist); ray_release(tbl);
        }
    }
    PASS();
}

static int wide_test_code(ray_t* vector, int64_t row, const int64_t* symbols) {
    if (ray_vec_is_null(vector, row)) return 0;
    if (vector->type == RAY_F32) return (int)((float*)ray_data(vector))[row];
    if (vector->type == RAY_GUID) return ((uint8_t*)ray_data(vector))[row * 16];
    if (vector->type == RAY_SYM) {
        int64_t code = ray_read_sym(ray_data(vector), row, vector->type, vector->attrs);
        for (int i = 1; i <= 3; i++) if (code == symbols[i]) return i;
        return -1;
    }
    size_t len = 0; const char* text = ray_str_vec_get(vector, row, &len);
    return len ? text[len - 1] - 'a' + 1 : 0;
}
static test_result_t test_parallel_wide_consumers(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    const int64_t n = 131072, ng = 4096;
    const int8_t types[] = {RAY_F32, RAY_GUID, RAY_SYM, RAY_STR};
    int64_t symbols[4] = {0, ray_sym_intern("wide-a", 6), ray_sym_intern("wide-b", 6), ray_sym_intern("wide-c", 6)};
    int64_t* rows = ray_alloc_raw((size_t)n * sizeof(int64_t));
    int64_t* offsets = ray_alloc_raw((size_t)(ng + 1) * sizeof(int64_t));
    int64_t* counts = ray_alloc_raw((size_t)ng * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(rows); TEST_ASSERT_NOT_NULL(offsets); TEST_ASSERT_NOT_NULL(counts);
    for (int64_t i = 0; i < n; i++) rows[i] = i;
    for (int64_t g = 0; g <= ng; g++) { offsets[g] = g * 32; if (g < ng) counts[g] = 32; }
    for (size_t ti = 0; ti < sizeof(types) / sizeof(types[0]); ti++) {
        int8_t type = types[ti];
        ray_t* src = type == RAY_SYM ? ray_sym_vec_new(RAY_SYM_W64, n) : ray_vec_new(type, n);
        TEST_ASSERT_NOT_NULL(src); src->len = n;
        src->attrs |= RAY_ATTR_HAS_NULLS;
        for (int64_t i = 0; i < n; i++) {
            int code = i / 32 == 17 || i % 5 == 0 ? 0 : (int)(i % 3) + 1;
            if (type == RAY_F32) ((float*)ray_data(src))[i] = code ? (float)code : NAN;
            else if (type == RAY_GUID) { memset((char*)ray_data(src) + i * 16, 0, 16); ((uint8_t*)ray_data(src))[i * 16] = code; }
            else if (type == RAY_SYM) ray_write_sym(ray_data(src), i, symbols[code], type, src->attrs);
            else {
                char text[] = "long pooled wide value a"; text[sizeof(text) - 2] += code ? code - 1 : 0;
                src = ray_str_vec_set(src, i, text, code ? sizeof(text) - 1 : 0);
                TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_FALSE(RAY_IS_ERR(src));
            }
        }
        ray_t* mode = ray_mode_per_group_buf(src, rows, offsets, counts, ng);
        TEST_ASSERT_NOT_NULL(mode); TEST_ASSERT_FALSE(RAY_IS_ERR(mode));
        for (int64_t g = 0; g < ng; g++) {
            int hist[4] = {0}, first[4] = {32, 32, 32, 32};
            for (int j = 0; j < 32; j++) {
                int64_t row = g * 32 + j;
                int code = g == 17 || row % 5 == 0 ? 0 : (int)(row % 3) + 1;
                hist[code]++; if (first[code] == 32) first[code] = j;
            }
            int best = 0;
            for (int c = 1; c <= 3; c++) if (hist[c] > hist[best] || (hist[c] == hist[best] && first[c] < first[best])) best = c;
            TEST_ASSERT_EQ_I(wide_test_code(mode, g, symbols), best);
        }
        ray_release(mode);
        for (int desc = 0; desc < 2; desc++) {
            ray_t* top = ray_topk_per_group_buf(src, 3, desc, rows, offsets, counts, ng);
            TEST_ASSERT_NOT_NULL(top); TEST_ASSERT_FALSE(RAY_IS_ERR(top));
            TEST_ASSERT_EQ_I(top->len, ng);
            for (int64_t g = 0; g < ng; g++) {
                ray_t* cell = ray_list_get(top, g);
                TEST_ASSERT_NOT_NULL(cell); TEST_ASSERT_EQ_I(cell->type, type);
                TEST_ASSERT_EQ_I(cell->len, g == 17 ? 0 : 3);
                for (int64_t i = 0; i < cell->len; i++) TEST_ASSERT_EQ_I(wide_test_code(cell, i, symbols), desc ? 3 : 1);
            }
            ray_release(top);
            if (type == RAY_GUID || type == RAY_STR) {
                ray_t* extreme = ray_wide_minmax_per_group_buf(src, desc ? OP_MAX : OP_MIN, rows, offsets, counts, ng);
                TEST_ASSERT_NOT_NULL(extreme); TEST_ASSERT_FALSE(RAY_IS_ERR(extreme));
                for (int64_t g = 0; g < ng; g++) TEST_ASSERT_EQ_I(wide_test_code(extreme, g, symbols), g == 17 ? 0 : desc ? 3 : 1);
                ray_release(extreme);
            }
        }
        ray_release(src);
    }
    ray_free_raw(rows); ray_free_raw(offsets); ray_free_raw(counts);
    PASS();
}

/* Histogram ranks independently check both sides of the parallel grain. */
static test_result_t test_parallel_rank_size(int64_t n) {
    const int64_t counts[] = {n / 2, n / 2 - 17, 17};
    const int64_t offsets[] = {0, n / 2, n - 17};
    const int8_t types[] = {RAY_I64, RAY_F32, RAY_TIME, RAY_BOOL};
    const double probabilities[] = {0.0, 0.25, 0.5, 0.9, 1.0};
    int64_t* rows = ray_alloc_raw((size_t)n * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(rows);
    for (int64_t i = 0; i < n; i++) rows[i] = i;
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        for (int shape = 0; shape < 2; shape++) {
            ray_t* values = ray_vec_new(types[t], n);
            TEST_ASSERT_NOT_NULL(values); values->len = n;
            bool nullable = types[t] != RAY_BOOL;
            if (nullable) values->attrs |= RAY_ATTR_HAS_NULLS;
            int64_t histogram[3][257] = {{0}}, valid[3] = {0};
            for (int64_t g = 0; g < 3; g++) {
                for (int64_t j = 0; j < counts[g]; j++) {
                    int64_t row = offsets[g] + j;
                    int code = shape ? 256 - (int)(j * 257 / counts[g]) : (int)(row * 37 % 257);
                    if (types[t] == RAY_BOOL) code %= 2;
                    bool missing = nullable && (row % 97 == 0 || (shape && g == 1));
                    if (!missing) { histogram[g][code]++; valid[g]++; }
                    if (types[t] == RAY_I64) ((int64_t*)ray_data(values))[row] = missing ? NULL_I64 : code;
                    else if (types[t] == RAY_TIME) ((int32_t*)ray_data(values))[row] = missing ? NULL_I32 : code;
                    else if (types[t] == RAY_F32) ((float*)ray_data(values))[row] = missing ? NULL_F32 : code + 0.25f;
                    else ((uint8_t*)ray_data(values))[row] = code;
                }
            }
            for (int op = -1; op < 5; op++) {
                ray_t* out = op < 0 ? ray_median_per_group_buf(values, rows, offsets, counts, 3)
                    : ray_quantile_per_group_buf(values, rows, offsets, counts, 3, probabilities[op]);
                TEST_ASSERT_FMT(out && !RAY_IS_ERR(out), "dominant rank failed");
                TEST_ASSERT_EQ_I(out->type, RAY_F64);
                for (int64_t g = 0; g < 3; g++) {
                    if (!valid[g]) { TEST_ASSERT_TRUE(ray_vec_is_null(out, g)); continue; }
                    double position = (op < 0 ? 0.5 : probabilities[op]) * (double)(valid[g] - 1);
                    int64_t lo = (int64_t)position, hi = position > lo ? lo + 1 : lo;
                    int64_t seen = 0;
                    double lower = 0, upper = 0;
                    for (int code = 0; code <= 256; code++) {
                        double value = code + (types[t] == RAY_F32 ? 0.25 : 0.0);
                        if (seen <= lo && lo < seen + histogram[g][code]) lower = value;
                        if (seen <= hi && hi < seen + histogram[g][code]) upper = value;
                        seen += histogram[g][code];
                    }
                    double expected = lower + (position - lo) * (upper - lower);
                    TEST_ASSERT_EQ_F(((double*)ray_data(out))[g], expected, 1e-12);
                }
                ray_release(out);
            }
            ray_release(values);
        }
    }
    ray_free_raw(rows);
    PASS();
}

static test_result_t test_parallel_rank_dominant(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    const int64_t sizes[] = {131072, 400000, 1048576};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        test_result_t result = test_parallel_rank_size(sizes[i]);
        if (result.status != TEST_PASS) return result;
    }
    PASS();
}

/* Independent long-double moments and pair sums validate reassociation in
 * both partitioned streaming and shared indexed consumers. */
static test_result_t test_parallel_float_oracle(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    const int64_t n = 262144, ng = 4096;
    long double sx[4096] = {0}, xx[4096] = {0}, xy[4096] = {0}, weights[4096] = {0};
    int64_t nx[4096] = {0}, np[4096] = {0};
    ray_t* keys = ray_vec_new(RAY_I32, n);
    ray_t* x = ray_vec_new(RAY_F64, n);
    ray_t* y = ray_vec_new(RAY_F64, n);
    TEST_ASSERT_NOT_NULL(keys); TEST_ASSERT_NOT_NULL(x); TEST_ASSERT_NOT_NULL(y);
    keys->len = x->len = y->len = n;
    x->attrs |= RAY_ATTR_HAS_NULLS; y->attrs |= RAY_ATTR_HAS_NULLS;
    for (int64_t i = 0; i < n; i++) {
        int64_t g = i % 3 == 0 ? 0 : i % ng;
        double a = (i % 101 - 50) * 0.25, b = (i % 31 + 1) * 0.125;
        bool na = i % 97 == 0 || g == 17, nb = i % 103 == 0;
        ((int32_t*)ray_data(keys))[i] = g;
        ((double*)ray_data(x))[i] = na ? NAN : a;
        ((double*)ray_data(y))[i] = nb ? NAN : b;
        if (!na) { nx[g]++; sx[g] += a; xx[g] += (long double)a * a; }
        if (!na && !nb) { np[g]++; xy[g] += (long double)a * b; weights[g] += a; }
    }
    ray_t* t = ray_table_new(3);
    t = ray_table_add_col(t, ray_sym_intern("k", 1), keys);
    t = ray_table_add_col(t, ray_sym_intern("x", 1), x);
    t = ray_table_add_col(t, ray_sym_intern("y", 1), y);
    ray_env_set(ray_sym_intern("t", 1), t);
    ray_release(t); ray_release(keys); ray_release(x); ray_release(y);
    for (int indexed = 0; indexed < 2; indexed++) {
        ray_t* out = ray_eval_str(indexed
            ? "(select {from:t by:k s:(sum x) a:(avg x) v:(var_pop x) w:(wsum x y) wa:(wavg x y) m:(med x)})"
            : "(select {from:t by:k s:(sum x) a:(avg x) v:(var_pop x) w:(wsum x y) wa:(wavg x y)})");
        TEST_ASSERT_FMT(out && !RAY_IS_ERR(out), "floating oracle query failed");
        TEST_ASSERT_EQ_I(ray_table_nrows(out), ng);
        int32_t* groups = ray_data(ray_table_get_col_idx(out, 0));
        for (int64_t r = 0; r < ng; r++) {
            int64_t g = groups[r];
            TEST_ASSERT_TRUE(g >= 0 && g < ng);
            long double mean = nx[g] ? sx[g] / nx[g] : 0;
            long double expected[] = {sx[g], mean, nx[g] ? xx[g] / nx[g] - mean * mean : 0,
                                      xy[g], weights[g] ? xy[g] / weights[g] : 0};
            for (int a = 0; a < 5; a++) {
                ray_t* col = ray_table_get_col_idx(out, a + 1);
                TEST_ASSERT_EQ_I(col->type, RAY_F64);
                if ((a == 1 || a == 2) && !nx[g]) { TEST_ASSERT_TRUE(ray_vec_is_null(col, r)); continue; }
                if (a == 4 && (!np[g] || !weights[g])) { TEST_ASSERT_TRUE(ray_vec_is_null(col, r)); continue; }
                double actual = ((double*)ray_data(col))[r];
                TEST_ASSERT_FMT(fabsl((long double)actual - expected[a]) <= 1e-9L + fabsl(expected[a]) * 1e-10L,
                    "float oracle indexed=%d group=%lld aggregate=%d got=%.17g expected=%.17Lg",
                    indexed, (long long)g, a, actual, expected[a]);
            }
        }
        ray_release(out);
    }
    PASS();
}

static test_result_t test_parallel_selected_composite(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4), RAY_OK);
    ray_t* setup = ray_eval_str(
        "(set composite_i (til 262144)) "
        "(set composite_t (table [a b v] (list (as 'I32 (* (% composite_i 4096) 4)) "
        "(as 'I16 (% (div composite_i 4096) 2)) (- (% composite_i 101) 50)))) "
        "(set composite_t (update {from:composite_t a:(as 'I32 0N) where:(== (% v 7) 0)}))");
    TEST_ASSERT_NOT_NULL(setup); TEST_ASSERT_FALSE(RAY_IS_ERR(setup)); ray_release(setup);
    int64_t counts[8194] = {0}, sums[8194] = {0}, minima[8194];
    bool seen[8194] = {0};
    for (int k = 0; k < 8194; k++) minima[k] = INT64_MAX;
    int groups = 0;
    for (int64_t r = 0; r < 262144; r++) {
        int64_t v = r % 101 - 50;
        if (v <= 0) continue;
        int key = (r / 4096 % 2) * 4097 + (v % 7 == 0 ? 4096 : r % 4096);
        if (!counts[key]) groups++;
        counts[key]++; sums[key] += v;
        if (v < minima[key]) minima[key] = v;
    }
    agg_route_reset();
    ray_t* out = ray_eval_str("(select {from:composite_t by:[a b] s:(sum v) lo:(min v) n:(count v) where:(> v 0)})");
    TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
    TEST_ASSERT_EQ_I(agg_route_stats().dense_strategy, AGG_DENSE_PARTITIONED);
    TEST_ASSERT_EQ_I(ray_table_nrows(out), groups);
    ray_t* a = ray_table_get_col_idx(out, 0); ray_t* b = ray_table_get_col_idx(out, 1);
    TEST_ASSERT_EQ_I(a->type, RAY_I32); TEST_ASSERT_EQ_I(b->type, RAY_I16);
    for (int64_t r = 0; r < groups; r++) {
        int32_t x = ((int32_t*)ray_data(a))[r]; int16_t y = ((int16_t*)ray_data(b))[r];
        int key = y * 4097 + (x == NULL_I32 ? 4096 : x / 4);
        TEST_ASSERT_TRUE(key >= 0 && key < 8194); TEST_ASSERT_FALSE(seen[key]); seen[key] = true;
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 2)))[r], sums[key]);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 3)))[r], minima[key]);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(out, 4)))[r], counts[key]);
    }
    ray_release(out);
    out = ray_eval_str("(select {from:composite_t by:[a b] s:(sum v) where:(> v 100)})");
    TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out)); TEST_ASSERT_EQ_I(ray_table_nrows(out), 0);
    ray_release(out);
    PASS();
}

static test_result_t test_parallel_native_gather(void) {
    const int8_t types[] = {RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64,
        RAY_F32, RAY_F64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_GUID, RAY_SYM};
    const uint8_t widths[] = {RAY_SYM_W8, RAY_SYM_W16, RAY_SYM_W32, RAY_SYM_W64};
    const int64_t n = 131072;
    int64_t* rows = ray_alloc_raw((size_t)n * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(rows);
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        int8_t type = types[t];
        bool nullable = type != RAY_BOOL && type != RAY_U8;
        for (unsigned w = 0; w < (type == RAY_SYM ? 4u : 1u); w++) {
            ray_t* source = type == RAY_SYM ? ray_sym_vec_new(widths[w], 4) : contract_fixture(type);
            TEST_ASSERT_NOT_NULL(source); TEST_ASSERT_FALSE(RAY_IS_ERR(source));
            if (type == RAY_SYM) {
                source->len = 4;
                for (int64_t i = 0; i < 4; i++) ray_write_sym(ray_data(source), i, i + 1, type, source->attrs);
            }
            if (nullable) ray_vec_set_null(source, 2, true);
            ray_t* view = ray_vec_slice(source, 1, 3);
            TEST_ASSERT_NOT_NULL(view); TEST_ASSERT_FALSE(RAY_IS_ERR(view));
            /* Null metadata inherited from a sliced parent must survive. */
            view->attrs &= ~RAY_ATTR_HAS_NULLS;
            for (int64_t i = 0; i < n; i++) rows[i] = nullable && i % 7 == 0 ? -1 : i * 17 % 3;
            ray_t* out = ray_group_gather(view, rows, n);
            TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
            TEST_ASSERT_EQ_I(out->type, type); TEST_ASSERT_EQ_I(out->len, n);
            size_t width = col_esz(view);
            if (type == RAY_SYM) {
                TEST_ASSERT_EQ_I(out->attrs & RAY_SYM_W_MASK, widths[w]);
                TEST_ASSERT_TRUE(ray_sym_vec_domain(out) == ray_sym_vec_domain(source));
            }
            for (int64_t i = 0; i < n; i++) {
                bool missing = rows[i] < 0 || (nullable && rows[i] == 1);
                TEST_ASSERT_FMT(ray_vec_is_null(out, i) == missing,
                    "gather %s width=%zu output=%lld source=%lld: null mismatch",
                    ray_type_name(type), width, (long long)i, (long long)rows[i]);
                if (rows[i] >= 0)
                    TEST_ASSERT_TRUE(memcmp((char*)ray_data(out) + (size_t)i * width,
                        (char*)ray_data(source) + (size_t)(rows[i] + 1) * width, width) == 0);
            }
            ray_t* empty = ray_group_gather(view, NULL, 0);
            TEST_ASSERT_NOT_NULL(empty); TEST_ASSERT_FALSE(RAY_IS_ERR(empty));
            TEST_ASSERT_EQ_I(empty->type, type); TEST_ASSERT_EQ_I(empty->len, 0);
            ray_release(empty); ray_release(out); ray_release(view); ray_release(source);
        }
    }
    ray_free_raw(rows);
    PASS();
}

static bool contract_same_cell(ray_t* out, int64_t at, ray_t* src, int64_t row) {
    if (row < 0) return src->type == RAY_BOOL || src->type == RAY_U8
        ? ((uint8_t*)ray_data(out))[at] == 0 : ray_vec_is_null(out, at);
    if (src->type == RAY_STR) {
        size_t na = 0, nb = 0;
        const char* a = ray_str_vec_get(out, at, &na);
        const char* b = ray_str_vec_get(src, row, &nb);
        return na == nb && (!na || memcmp(a, b, na) == 0);
    }
    size_t width = col_esz(src);
    return memcmp((char*)ray_data(out) + (size_t)at * width,
                  (char*)ray_data(src) + (size_t)row * width, width) == 0;
}
static test_result_t test_parallel_dominant_consumers(void) {
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(3), RAY_OK);
    const int64_t n = 524288, counts[] = {262144, 262144, 17, 0};
    const int64_t offsets[] = {0, 262144, n, n + 17};
    const int8_t types[] = {RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64,
        RAY_F32, RAY_F64, RAY_DATE, RAY_TIME, RAY_TIMESTAMP, RAY_GUID, RAY_SYM, RAY_STR};
    int64_t* rows = ray_alloc_raw((size_t)(n + 17) * sizeof(int64_t));
    TEST_ASSERT_NOT_NULL(rows);
    /* Deliberately not source-row order: tie priority follows index order. */
    for (int64_t i = 0; i < n + 17; i++) rows[i] = i < n / 2 ? (i % 2 ? 1 : 3) : i < n ? 2 : 0;
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        int8_t type = types[t];
        bool nullable = type != RAY_BOOL && type != RAY_U8;
        ray_t* src = contract_fixture(type);
        TEST_ASSERT_NOT_NULL(src); TEST_ASSERT_FALSE(RAY_IS_ERR(src));
        if (type == RAY_GUID) {
            memset(ray_data(src), 0, 64);
            for (int i = 0; i < 4; i++) ((uint8_t*)ray_data(src))[16 * i] = i + 1;
        }
        if (nullable) ray_vec_set_null(src, 2, true);
        if (type == RAY_I64 || type == RAY_F64) {
            const int64_t tiny_rows[] = {3, 2, 1, 0}, off = 0, count = 4;
            for (int desc = 0; desc < 2; desc++) {
                ray_t* tiny = ray_topk_per_group_buf(src, 10, desc, tiny_rows, &off, &count, 1);
                TEST_ASSERT_NOT_NULL(tiny); TEST_ASSERT_FALSE(RAY_IS_ERR(tiny));
                ray_t* cell = ray_list_get(tiny, 0);
                const int64_t expected_rows[] = {0, 1, 3};
                TEST_ASSERT_EQ_I(cell->len, 3);
                for (int i = 0; i < 3; i++)
                    TEST_ASSERT_TRUE(contract_same_cell(cell, i, src, expected_rows[desc ? 2 - i : i]));
                ray_release(tiny);
            }
        }
        int64_t expected[] = {3, nullable && type != RAY_STR ? -1 : 2, 0, -1};
        ray_t* mode = ray_mode_per_group_buf(src, rows, offsets, counts, 4);
        TEST_ASSERT_NOT_NULL(mode); TEST_ASSERT_FALSE(RAY_IS_ERR(mode));
        TEST_ASSERT_EQ_I(mode->type, type); TEST_ASSERT_EQ_I(mode->len, 4);
        for (int g = 0; g < 4; g++)
            TEST_ASSERT_FMT(contract_same_cell(mode, g, src, expected[g]),
                "dominant mode %s group %d", ray_type_name(type), g);
        ray_release(mode);
        const int64_t ks[] = {3, 10000, 300000};
        for (int ki = 0; ki < 3; ki++) for (int desc = 0; desc < 2; desc++) {
            ray_t* top = ray_topk_per_group_buf(src, ks[ki], desc, rows, offsets, counts, 4);
            TEST_ASSERT_NOT_NULL(top); TEST_ASSERT_FALSE(RAY_IS_ERR(top));
            for (int g = 0; g < 4; g++) {
                ray_t* cell = ray_list_get(top, g);
                TEST_ASSERT_NOT_NULL(cell); TEST_ASSERT_EQ_I(cell->type, type);
                int64_t length = g == 3 || (g == 1 && nullable) ? 0 : counts[g] < ks[ki] ? counts[g] : ks[ki];
                TEST_ASSERT_EQ_I(cell->len, length);
                for (int64_t j = 0; j < length; j++)
                    TEST_ASSERT_FMT(contract_same_cell(cell, j, src, g == 0 ? ((desc == (j < counts[g] / 2)) ? 3 : 1) : g == 1 ? 2 : 0),
                        "dominant top/bot %s group %d k=%lld desc=%d row=%lld", ray_type_name(type), g, (long long)ks[ki], desc, (long long)j);
            }
            ray_release(top);
        }
        if (type == RAY_GUID || type == RAY_STR) {
            const uint16_t ops[] = {OP_MIN, OP_MAX, OP_FIRST, OP_LAST};
            const int64_t first_group[] = {1, 3, 3, 1};
            for (int op = 0; op < 4; op++) {
                ray_t* out = ray_wide_minmax_per_group_buf(src, ops[op], rows, offsets, counts, 4);
                TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_FALSE(RAY_IS_ERR(out));
                for (int g = 0; g < 4; g++)
                    TEST_ASSERT_TRUE(contract_same_cell(out, g, src, g == 0 ? first_group[op] : g == 2 ? 0 : -1));
                ray_release(out);
            }
        }
        ray_release(src);
    }
    /* Many values force local collisions and exact partition merging. All
     * frequencies tie, so the first position must beat the smallest row id. */
    ray_t* values = ray_vec_new(RAY_I64, n);
    TEST_ASSERT_NOT_NULL(values); values->len = n;
    for (int64_t i = 0; i < n; i++) {
        ((int64_t*)ray_data(values))[i] = i % 4096;
        rows[i] = n - 1 - i;
    }
    const int64_t zero = 0;
    ray_t* mode = ray_mode_per_group_buf(values, rows, &zero, &n, 1);
    TEST_ASSERT_NOT_NULL(mode); TEST_ASSERT_FALSE(RAY_IS_ERR(mode));
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(mode))[0], 4095);
    ray_release(mode);
    const int64_t uneven = n - 3, large_k = 100003;
    int64_t histogram[4096] = {0};
    for (int64_t i = 0; i < uneven; i++) histogram[rows[i] % 4096]++;
    for (int desc = 0; desc < 2; desc++) {
        ray_t* result = ray_topk_per_group_buf(values, large_k, desc, rows, &zero, &uneven, 1);
        TEST_ASSERT_NOT_NULL(result); TEST_ASSERT_FALSE(RAY_IS_ERR(result));
        ray_t* cell = ray_list_get(result, 0);
        TEST_ASSERT_EQ_I(cell->len, large_k);
        int64_t at = 0;
        for (int c = 0; c < 4096 && at < large_k; c++) {
            int value = desc ? 4095 - c : c;
            for (int64_t j = 0; j < histogram[value] && at < large_k; j++)
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(cell))[at++], value);
        }
        TEST_ASSERT_EQ_I(at, large_k);
        ray_release(result);
    }
    ray_release(values); ray_free_raw(rows);
    PASS();
}

/* Exercise inference failures without relying on allocator fault timing. */
static const char* empty_probe_error;
static ray_t* empty_probe(ray_t* input) {
    (void)input;
    return ray_error(empty_probe_error, "empty inference probe");
}
static test_result_t test_empty_inference_errors(void) {
    ray_t* fn = ray_fn_unary("empty_probe", RAY_FN_AGGR, empty_probe);
    TEST_ASSERT_NOT_NULL(fn);
    ray_env_set(ray_sym_intern("empty_probe", 11), fn);
    ray_release(fn);
    const char* codes[] = {"domain", "type", "oom", "cancel"};
    for (int i = 0; i < 4; i++) {
        empty_probe_error = codes[i];
        ray_t* out = ray_eval_str("(select {from:(table [k v] (list [1 2] [3 4])) by:k where:(< k 0) a:(empty_probe v)})");
        TEST_ASSERT_NOT_NULL(out);
        if (i < 2) {
            TEST_ASSERT_FALSE(RAY_IS_ERR(out));
            TEST_ASSERT_EQ_I(ray_table_nrows(out), 0);
            ray_t* col = ray_table_get_col(out, ray_sym_intern("a", 1));
            TEST_ASSERT_NOT_NULL(col);
            TEST_ASSERT_EQ_I(col->type, RAY_LIST);
        } else {
            TEST_ASSERT_TRUE(RAY_IS_ERR(out));
            TEST_ASSERT_STR_EQ(ray_err_code(out), codes[i]);
        }
        ray_release(out);
    }
    PASS();
}

static test_result_t test_nth_bounds(void) {
    double values[] = {3.0, 1.0, 2.0};
    TEST_ASSERT_TRUE(isnan(ray_nth_dbl_inplace(NULL, 0, 0)));
    TEST_ASSERT_TRUE(isnan(ray_nth_dbl_inplace(values, 0, 0)));
    TEST_ASSERT_TRUE(isnan(ray_nth_dbl_inplace(values, 3, -1)));
    TEST_ASSERT_TRUE(isnan(ray_nth_dbl_inplace(values, 3, 3)));
    TEST_ASSERT_TRUE(ray_nth_dbl_inplace(values, 3, 0) == 1.0);
    TEST_ASSERT_TRUE(ray_nth_dbl_inplace(values, 3, 2) == 3.0);
    PASS();
}

static test_result_t test_cancelled_group(void) {
    ray_t* tbl = ray_eval_str("(table [k v] (list [0 0 1 1] (as 'TIME [1 2 3 4])))");
    TEST_ASSERT_NOT_NULL(tbl); TEST_ASSERT_FALSE(RAY_IS_ERR(tbl));
    ray_graph_t* graph = ray_graph_new(tbl);
    ray_op_t* keys[] = {ray_scan(graph, "k")};
    ray_op_t* values[] = {ray_scan(graph, "v")};
    uint16_t kind = OP_MIN;
    ray_op_t* group = ray_group(graph, keys, 1, &kind, values, 1);
    ray_pool_t* pool = ray_pool_get();
    atomic_store_explicit(&pool->cancelled, 1, memory_order_relaxed);
    ray_t* out = exec_group_v2(graph, group, tbl, 0);
    atomic_store_explicit(&pool->cancelled, 0, memory_order_relaxed);
    TEST_ASSERT_NOT_NULL(out); TEST_ASSERT_TRUE(RAY_IS_ERR(out));
    TEST_ASSERT_STR_EQ(ray_err_code(out), "cancel");
    ray_error_free(out); ray_graph_free(graph); ray_release(tbl);
    PASS();
}

/* Replicated task-local slabs are bounded by the last-level cache: with a
 * 20-worker pool and a 100k-slot slab the raw replication (20 slabs) leaves
 * most caches, and the run must use at most floor(0.75 * LLC / slab) task
 * slabs (never fewer than the pool when everything fits).  The result is
 * identical either way. */
static test_result_t test_dense_cache_bound(void) {
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(20), RAY_OK);
    ray_t* setup = ray_eval_str(
        "(set cb_i (til 4000000)) "
        "(set cb_t (table [k v] (list (as 'I32 (% (* cb_i 7919) 100000)) (% cb_i 13))))");
    TEST_ASSERT_NOT_NULL(setup); TEST_ASSERT_FALSE(RAY_IS_ERR(setup)); ray_release(setup);
    agg_route_reset();
    ray_t* r = ray_eval_str("(select {from:cb_t by:k s:(sum v)})");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    agg_route_stats_t stats = agg_route_stats();
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_DENSE], 1);
    TEST_ASSERT_EQ_I(stats.dense_strategy, AGG_DENSE_TASK_LOCAL);
    TEST_ASSERT_TRUE(stats.dense_tasks >= 2 && stats.dense_tasks <= 20);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 100000);
    uint64_t llc = ray_cache_llc_bytes();
    if (llc > 0) {
        size_t block = agg_resolve(OP_SUM, RAY_I64)->state_size;
        double slots = (double)stats.dense_local_slots / stats.dense_tasks;
        double slab = slots * (block + sizeof(int64_t) + 1);
        double budget = (double)llc * 0.75;
        uint32_t cap = slab * 20 > budget ? (uint32_t)(budget / slab) : 20;
        if (cap < 2) cap = 2;
        TEST_ASSERT_EQ_I(stats.dense_tasks, cap);
    }
    /* The bounded run computes the same sums as the serial engine. */
    ray_t* check = ray_eval_str(
        "(all (== (at (xasc (select {from:cb_t by:k s:(sum v)}) 'k) 's) "
        "(at (xasc (select {from:(select {from:cb_t k:k v:v}) by:k s:(sum v)}) 'k) 's)))");
    TEST_ASSERT_NOT_NULL(check); TEST_ASSERT_FALSE(RAY_IS_ERR(check));
    TEST_ASSERT_EQ_I(check->i64, 1);
    ray_release(check); ray_release(r);
    ray_release(ray_eval_str("(set cb_t 0) (set cb_i 0)"));
    PASS();
}

/* Composite symbol keys whose codes interleave in the shared domain get a
 * compacted dense plan: two 40-value keys over a 4,000-code domain pack into
 * 1,600 slots instead of falling to radix. */
static test_result_t test_dense_composite_compaction(void) {
    ray_t* setup = ray_eval_str(
        "(set cc_i (til 400000)) "
        "(set cc_syms (as 'SYM (map (fn [x] (format \"s%\" (+ 10000 x))) (til 4000)))) "
        "(set cc_a (at cc_syms (* (% cc_i 40) 100))) "
        "(set cc_b (at cc_syms (+ 1 (* (% (div cc_i 40) 40) 100)))) "
        "(set cc_t (table [a b v] (list cc_a cc_b (% cc_i 11))))");
    TEST_ASSERT_NOT_NULL(setup); TEST_ASSERT_FALSE(RAY_IS_ERR(setup)); ray_release(setup);
    agg_route_reset();
    ray_t* r = ray_eval_str("(select {from:cc_t by:[a b] s:(sum v) n:(count v)})");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    agg_route_stats_t stats = agg_route_stats();
    TEST_ASSERT_TRUE(stats.dense_plan_available);
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_DENSE], 1);
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_RADIX], 0);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 1600);
    ray_t* check = ray_eval_str(
        "(== (sum (at (select {from:cc_t by:[a b] s:(sum v)}) 's)) (sum (at cc_t 'v)))");
    TEST_ASSERT_NOT_NULL(check); TEST_ASSERT_FALSE(RAY_IS_ERR(check));
    TEST_ASSERT_EQ_I(check->i64, 1);
    ray_release(check); ray_release(r);
    ray_release(ray_eval_str("(set cc_t 0) (set cc_a 0) (set cc_b 0) (set cc_syms 0) (set cc_i 0)"));
    PASS();
}

/* A many-million-group top-N must stay on the parallel engine and emit only
 * the kept superset: radix selects by aggregate value per partition. */
static test_result_t test_radix_native_topn(void) {
    ray_pool_destroy();
    TEST_ASSERT_EQ_I(ray_pool_init_total(8), RAY_OK);
    ray_t* setup = ray_eval_str(
        "(set rt_i (til 2000000)) "
        "(set rt_t (table [k j v] (list (% (* rt_i 7919) 1500000) (% rt_i 3) (% rt_i 5))))");
    TEST_ASSERT_NOT_NULL(setup); TEST_ASSERT_FALSE(RAY_IS_ERR(setup)); ray_release(setup);
    agg_route_reset();
    ray_t* r = ray_eval_str("(select {from:rt_t by:[k j] c:(count v) desc:c take:10})");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    agg_route_stats_t stats = agg_route_stats();
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_LEGACY], 0);
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_V2_RADIX], 1);
    TEST_ASSERT_TRUE(stats.topn_native);
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 10);
    ray_t* check = ray_eval_str(
        "(== (at (at (select {from:rt_t by:[k j] c:(count v) desc:c take:10}) 'c) 0) "
        "(max (at (select {from:rt_t by:[k j] c:(count v)}) 'c)))");
    TEST_ASSERT_NOT_NULL(check); TEST_ASSERT_FALSE(RAY_IS_ERR(check));
    TEST_ASSERT_EQ_I(check->i64, 1);
    ray_release(check); ray_release(r);
    /* asc keeps the smallest counts */
    r = ray_eval_str("(select {from:rt_t by:[k j] c:(count v) asc:c take:3})");
    TEST_ASSERT_NOT_NULL(r); TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(ray_table_nrows(r), 3);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(r, 2)))[0], 1);
    ray_release(r);
    ray_release(ray_eval_str("(set rt_t 0) (set rt_i 0)"));
    PASS();
}

const test_entry_t agg_contract_entries[] = {
    { "agg_contract/empty_inference_errors", test_empty_inference_errors, contract_setup, contract_teardown },
    { "agg_contract/nth_bounds", test_nth_bounds, contract_setup, contract_teardown },
    { "agg_contract/native_binary_output", test_native_binary_output, contract_setup, contract_teardown },
    { "agg_contract/native_streaming_output", test_native_streaming_output, contract_setup, contract_teardown },
    { "agg_contract/unary_types_values", test_unary_contracts, contract_setup, contract_teardown },
    { "agg_contract/pairwise_numeric", test_pairwise_numeric_contracts, contract_setup, contract_teardown },
    { "agg_contract/registry_admission", test_registry_admission_contracts, contract_setup, contract_teardown },
    { "agg_contract/routes_bool_outputs", test_group_routes_and_bool_outputs, contract_setup, contract_teardown },
    { "agg_contract/derived_key_per_symbol_route", test_derived_key_per_symbol_route, contract_setup, contract_teardown },
    { "agg_contract/narrow_extrema_limits", test_narrow_extrema_limits, contract_setup, contract_teardown },
    { "agg_contract/dense_strategies", test_dense_strategies, contract_setup, contract_teardown },
    { "agg_contract/dense_symbol_output", test_dense_symbol_output, contract_setup, contract_teardown },
    { "agg_contract/dense_task_local", test_dense_task_local, contract_setup, contract_teardown },
    { "agg_contract/dense_cache_bound", test_dense_cache_bound, contract_setup, contract_teardown },
    { "agg_contract/dense_composite_compaction", test_dense_composite_compaction, contract_setup, contract_teardown },
    { "agg_contract/radix_native_topn", test_radix_native_topn, contract_setup, contract_teardown },
    { "agg_contract/rank_widths_nulls_slices", test_rank_widths_nulls_and_slices, contract_setup, contract_teardown },
    { "agg_contract/nullable_differential", test_nullable_differential, contract_setup, contract_teardown },
    { "agg_contract/wide_key_routes", test_wide_key_routes, contract_setup, contract_teardown },
    { "agg_contract/fused_typed_comparisons", test_fused_typed_comparisons, contract_setup, contract_teardown },
    { "agg_contract/count_distinct_typed_routes", test_count_distinct_typed_routes, contract_setup, contract_teardown },
    { "agg_contract/indexed_parallel_layout", test_indexed_parallel_layout, contract_setup, contract_teardown },
    { "agg_contract/wide_count_distinct", test_wide_count_distinct, contract_setup, contract_teardown },
    { "agg_contract/parallel_wide_consumers", test_parallel_wide_consumers, contract_setup, contract_teardown },
    { "agg_contract/parallel_rank_dominant", test_parallel_rank_dominant, contract_setup, contract_teardown },
    { "agg_contract/parallel_float_oracle", test_parallel_float_oracle, contract_setup, contract_teardown },
    { "agg_contract/parallel_selected_composite", test_parallel_selected_composite, contract_setup, contract_teardown },
    { "agg_contract/parallel_native_gather", test_parallel_native_gather, contract_setup, contract_teardown },
    { "agg_contract/parallel_dominant_consumers", test_parallel_dominant_consumers, contract_setup, contract_teardown },
    { "agg_contract/cancelled_group", test_cancelled_group, contract_setup, contract_teardown },
    { NULL, NULL, NULL, NULL },
};
