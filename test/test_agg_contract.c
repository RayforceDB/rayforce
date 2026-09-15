/* Scalar/grouped semantic contracts, independent of optimized admission.
 * Baseline census: docs/aggregation-type-census.csv. Extend this table when
 * language semantics change; adding a vtable alone must not change it. */
#include "test.h"
#include "test_rfl.h"
#include "ops/agg_engine.h"
#include "ops/agg_registry.h"
#include "core/pool.h"
#include "lang/internal.h"
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
                type == RAY_I32 || type == RAY_I64 || type == RAY_F64;
            bool wide_numeric = type == RAY_I64 || type == RAY_F64;
            bool binary = agg_is_binary_agg(op);
            bool buffered = op == OP_MEDIAN || op == OP_TOP_N || op == OP_BOT_N;
            bool registered = op == OP_COUNT ||
                ((op == OP_SUM || op == OP_MIN || op == OP_MAX ||
                  op == OP_VAR || op == OP_VAR_POP || op == OP_STDDEV ||
                  op == OP_STDDEV_POP || buffered) && wide_numeric) ||
                (op == OP_AVG && type == RAY_F64) ||
                ((op == OP_ALL || op == OP_ANY || binary) && numeric);
            const agg_vtable_t* vt = agg_resolve(op, type);
            TEST_ASSERT_FMT((vt != NULL) == registered, "registry %u/%s changed", op, ray_type_name(type));
            ray_graph_t* graph = ray_graph_new(tbl);
            ray_op_t* keys[] = { ray_scan(graph, "k") };
            ray_op_t* ins[] = { ray_scan(graph, "v") };
            int64_t param[] = { op == OP_TOP_N || op == OP_BOT_N ? 2 : 0 };
            ray_op_t* group = ray_group_build(graph, keys, 1, &op, ins,
                                               binary ? ins : NULL, param, 1);
            agg_v2_reason_t want = !registered ? AGG_V2_AGG_TYPE
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
        { RAY_PARALLEL_THRESHOLD, 2, AGG_ROUTE_V2_HASH },
        { RAY_PARALLEL_THRESHOLD, 3, AGG_ROUTE_V2_RADIX },
    };
    for (size_t c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        int64_t n = cases[c].n;
        ray_t* k = ray_vec_new(RAY_I64, n); k->len = n;
        ray_t* v = ray_vec_new(RAY_BOOL, n); v->len = n;
        for (int64_t i = 0; i < n; i++) {
            int64_t key = cases[c].mode == 3 ? i : i % 4;
            ((int64_t*)ray_data(k))[i] = cases[c].mode == 1 ? key * 1000000 : key;
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
        TEST_ASSERT_EQ_I(stats.dense_worker_budget, cases[c].mode == 3);
        ray_t* out_keys = ray_table_get_col_idx(out, 0);
        for (int a = 1; a <= 2; a++) {
            ray_t* col = ray_table_get_col_idx(out, a);
            TEST_ASSERT_EQ_I(col->type, RAY_BOOL);
            for (int64_t i = 0; i < col->len; i++) {
                int64_t key = ((int64_t*)ray_data(out_keys))[i];
                if (key == NULL_I64) key = 0;
                if (cases[c].mode == 1) key /= 1000000;
                TEST_ASSERT_EQ_I(((uint8_t*)ray_data(col))[i], key % 2);
            }
        }
        ray_release(out); ray_graph_free(graph); ray_release(tbl);
    }
    agg_route_reset();
    ray_t* r = ray_eval_str("(select {from:(table [k v] (list [0 0 1 1] (as 'TIME [1 2 3 4]))) by:k s:(min v)})");
    TEST_ASSERT_FALSE(RAY_IS_ERR(r)); ray_release(r);
    agg_route_stats_t stats = agg_route_stats();
    TEST_ASSERT_EQ_I(stats.routes[AGG_ROUTE_LEGACY], 1);
    TEST_ASSERT_EQ_I(stats.last_v2_reason, AGG_V2_AGG_TYPE);
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

const test_entry_t agg_contract_entries[] = {
    { "agg_contract/unary_types_values", test_unary_contracts, contract_setup, contract_teardown },
    { "agg_contract/pairwise_numeric", test_pairwise_numeric_contracts, contract_setup, contract_teardown },
    { "agg_contract/registry_admission", test_registry_admission_contracts, contract_setup, contract_teardown },
    { "agg_contract/routes_bool_outputs", test_group_routes_and_bool_outputs, contract_setup, contract_teardown },
    { "agg_contract/rank_widths_nulls_slices", test_rank_widths_nulls_and_slices, contract_setup, contract_teardown },
    { NULL, NULL, NULL, NULL },
};
