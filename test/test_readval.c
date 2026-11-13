#include "test.h"
#include "lang/readval.h"
#include "lang/format.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "table/sym.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vec/str.h"
#include "table/table.h"

static void setup(void) { ray_heap_init(); (void)ray_sym_init(); }
static void teardown(void) { ray_sym_destroy(); ray_heap_direct_cache_drain(); ray_heap_destroy(); }

static bool is_err(ray_t* r, const char* code) {
    return RAY_IS_ERR(r) && strcmp(ray_err_code(r), code) == 0;
}
static void drop(ray_t* r) { if (RAY_IS_ERR(r)) ray_error_free(r); else ray_release(r); }

/* A document touching every construction path: atoms of each width, typed
 * vectors, STR with a pooled string, SYM and GUID vectors, LIST, DICT and
 * TABLE with symbol names, and a hand-written dict. */
static const char* KITCHEN =
    "(list 1 2h 3i 1.5e 2.5 0x07 true \"short\" \"a string well past the inline limit\" 'one "
    "2024.01.02 12:00:00.000 0D01:00:00 2024.01.02D01:02:03.000000004 0Nl null "
    "[1 2 3] [1h 2h] [1.5 0Nf] [true false] [\"\" \"x\" \"pooled string over twelve\"] "
    "['alpha 'beta] (as 'SYM (list \"with space\" \"\")) (as 'I64 []) (as 'SYM []) "
    "(as 'GUID \"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\") "
    "(as 'GUID (list (as 'GUID \"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\") 0Ng)) "
    "(dict ['k1 'k2] (list 1 \"v\")) {first: 1 second: [1 2]} "
    "(table (as 'SYM (list \"c1\" \"c2\")) (list [1 2] (list 1 \"x\"))) "
    "(table (as 'SYM (list \"c3\")) (list (as 'I64 []))))";

static test_result_t limit_edges(void) {
    ray_read_limits_t d = ray_read_limits_default();
    TEST_ASSERT_EQ_U(d.max_input, 8u << 20);
    TEST_ASSERT_EQ_U(d.max_depth, 128);
    TEST_ASSERT_EQ_U(d.max_nodes, 1000000);
    TEST_ASSERT_EQ_U(d.max_grammar, 1000000);
    TEST_ASSERT_EQ_U(d.max_symbol_bytes, 1u << 20);
    TEST_ASSERT_EQ_U(d.max_alloc, 64u << 20);

    const char* text = "(list [1 2 3] (as 'SYM (list \"abcd\")))";
    size_t len = strlen(text);
    ray_read_limits_t l = d;
    ray_t* r;
    /* Each cap is inclusive: cap-1 fails, cap succeeds. */
    l.max_input = len - 1; r = ray_read_value(text, len, &l); TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    l.max_input = len;     r = ray_read_value(text, len, &l); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); drop(r);

    l = d; l.max_symbol_bytes = 3; r = ray_read_value(text, len, &l); TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    l.max_symbol_bytes = 4;        r = ray_read_value(text, len, &l); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); drop(r);

    /* depth: (list (list (list 1))) is depth 4 */
    const char* deep = "(list (list (list 1)))";
    l = d; l.max_depth = 3; r = ray_read_value(deep, strlen(deep), &l); TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    l.max_depth = 4;        r = ray_read_value(deep, strlen(deep), &l); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); drop(r);

    /* the fixed minimum of each counting budget is found, not assumed */
    size_t* caps[] = { &l.max_nodes, &l.max_grammar };
    for (size_t k = 0; k < 2; k++) {
        size_t lo = 0, hi = 1000;
        while (lo < hi) {
            size_t mid = (lo + hi) / 2;
            l = d; *caps[k] = 0; /* rebind pointer into the fresh struct */
            size_t* fld = k == 0 ? &l.max_nodes : &l.max_grammar;
            *fld = mid;
            r = ray_read_value(text, len, &l);
            bool ok = !RAY_IS_ERR(r);
            if (!ok) TEST_ASSERT_TRUE(is_err(r, "limit"));
            drop(r);
            if (ok) hi = mid; else lo = mid + 1;
        }
        TEST_ASSERT(lo > 0 && lo < 1000, "a minimum exists");
        l = d; (k == 0 ? &l.max_nodes : &l.max_grammar)[0] = lo - 1;
        r = ray_read_value(text, len, &l); TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
        l = d; (k == 0 ? &l.max_nodes : &l.max_grammar)[0] = lo;
        r = ray_read_value(text, len, &l); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); drop(r);
    }

    /* allocation: the minimum that succeeds, and one byte less that fails
     * with a limit error rather than an out-of-memory one */
    size_t lo = 0, hi = 1u << 20;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        l = d; l.max_alloc = mid;
        r = ray_read_value(KITCHEN, strlen(KITCHEN), &l);
        bool ok = !RAY_IS_ERR(r);
        if (!ok) TEST_ASSERT_TRUE(is_err(r, "limit"));
        drop(r);
        if (ok) hi = mid; else lo = mid + 1;
    }
    TEST_ASSERT(lo > 0 && lo < (1u << 20), "an allocation minimum exists");
    l = d; l.max_alloc = lo - 1; r = ray_read_value(KITCHEN, strlen(KITCHEN), &l); TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    l = d; l.max_alloc = lo;     r = ray_read_value(KITCHEN, strlen(KITCHEN), &l); TEST_ASSERT_FALSE(RAY_IS_ERR(r)); drop(r);
    PASS();
}

static test_result_t faults_leave_nothing(void) {
    size_t len = strlen(KITCHEN);
    /* warm the heap pools with a symbol-free document, so growth of the pools
     * on first use is not mistaken for a leak */
    const char* warm = "(list [1 2 3] \"a string well past the inline limit\" (as 'I64 []))";
    for (int i = 0; i < 3; i++) {
        ray_t* w = ray_read_value(warm, strlen(warm), NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(w));
        ray_release(w);
    }
    uint32_t syms = ray_sym_count();
    int64_t sys0;
    ray_sys_get_stat(&sys0, NULL);
    ray_t* want = NULL;
    bool done = false;
    for (size_t fail = 1; fail < 4000 && !done; fail++) {
        ray_read_limits_t l = ray_read_limits_default();
        l.inject_fail = fail;
        TEST_ASSERT_TRUE(ray_mem_trace_begin());
        ray_t* r = ray_read_value(KITCHEN, len, &l);
        if (RAY_IS_ERR(r)) {
            TEST_ASSERT_TRUE(is_err(r, "oom"));
            ray_error_free(r);
            /* a failed read leaves no heap block, no system block and no symbol */
            ray_mem_trace_t t;
            ray_mem_trace_end(&t);
            TEST_ASSERT_EQ_I(t.net_bytes, 0);
            int64_t sys1;
            ray_sys_get_stat(&sys1, NULL);
            TEST_ASSERT_EQ_I(sys1, sys0);
            TEST_ASSERT_EQ_U(ray_sym_count(), syms);
        } else {
            ray_mem_trace_t t;
            ray_mem_trace_end(&t);
            TEST_ASSERT_TRUE(ray_sym_count() > syms);
            want = r;
            done = true;
        }
    }
    TEST_ASSERT_TRUE(done);
    /* the result of the surviving read equals an uninjected one */
    ray_t* again = ray_read_value(KITCHEN, len, NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(again));
    ray_t* a = ray_fmt(want, RAY_FMT_ROUNDTRIP);
    ray_t* b = ray_fmt(again, RAY_FMT_ROUNDTRIP);
    TEST_ASSERT_EQ_U(ray_str_len(a), ray_str_len(b));
    TEST_ASSERT_TRUE(memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0);
    ray_release(a); ray_release(b); ray_release(again); ray_release(want);
    PASS();
}

static test_result_t release_returns_everything(void) {
    size_t len = strlen(KITCHEN);
    ray_t* warm = ray_read_value(KITCHEN, len, NULL);   /* intern the names once */
    TEST_ASSERT_FALSE(RAY_IS_ERR(warm));
    ray_release(warm);
    int64_t sys0;
    ray_sys_get_stat(&sys0, NULL);
    TEST_ASSERT_TRUE(ray_mem_trace_begin());
    ray_t* r = ray_read_value(KITCHEN, len, NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    ray_mem_trace_t t;
    ray_mem_trace_end(&t);
    TEST_ASSERT_EQ_I(t.net_bytes, 0);
    int64_t sys1;
    ray_sys_get_stat(&sys1, NULL);
    TEST_ASSERT_EQ_I(sys1, sys0);
    PASS();
}

static test_result_t rejected_reads_add_no_symbols(void) {
    static const char* bad[] = {
        "(list 'rejected_a (list 'rejected_b x))",
        "(table (as 'SYM (list \"rejected_c\")) (list [1] [2]))",
        "(list 'rejected_d 1) trailing",
        "{rejected_e: 1 rejected_f}",
        "(dict (as 'SYM (list \"rejected_g\")) [1 2])",
        "(list 'rejected_h \"unterminated",
    };
    uint32_t syms = ray_sym_count();
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        ray_t* r = ray_read_value(bad[i], strlen(bad[i]), NULL);
        TEST_ASSERT_TRUE(RAY_IS_ERR(r));
        drop(r);
        TEST_ASSERT_EQ_U(ray_sym_count(), syms);
    }
    TEST_ASSERT_EQ_I(ray_sym_find("rejected_a", 10), -1);
    TEST_ASSERT_EQ_I(ray_sym_find("rejected_c", 10), -1);
    /* a limit hit after the symbols were staged also publishes nothing */
    ray_read_limits_t l = ray_read_limits_default();
    l.max_nodes = 3;
    const char* t = "(list 'staged_a 'staged_b 1 2 3 4)";
    ray_t* r = ray_read_value(t, strlen(t), &l);
    TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    TEST_ASSERT_EQ_U(ray_sym_count(), syms);
    PASS();
}

static test_result_t symbols_commit_once_complete(void) {
    const char* t = "(list 'fresh_x (as 'SYM \"fresh y\") ['fresh_x 'fresh_z] {fresh_k: 1})";
    uint32_t syms = ray_sym_count();
    ray_t* r = ray_read_value(t, strlen(t), NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(ray_sym_count(), syms + 4);   /* x, "fresh y", z, k: each once */
    int64_t x = ray_sym_find("fresh_x", 7), y = ray_sym_find("fresh y", 7);
    int64_t z = ray_sym_find("fresh_z", 7), k = ray_sym_find("fresh_k", 7);
    TEST_ASSERT(x >= 0 && y >= 0 && z >= 0 && k >= 0, "all committed");
    ray_t** el = (ray_t**)ray_data(r);
    TEST_ASSERT_EQ_I(el[0]->i64, x);
    TEST_ASSERT_EQ_I(el[1]->i64, y);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(el[2]))[0], x);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(el[2]))[1], z);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_dict_keys(el[3])))[0], k);
    TEST_ASSERT_TRUE(el[0]->attrs & 0x20);   /* ATTR_QUOTED: a value, not a name */
    ray_release(r);
    /* a host ceiling at the current count refuses new names, atomically */
    ray_read_limits_t l = ray_read_limits_default();
    l.max_symbols = ray_sym_count();
    const char* t2 = "(list 'ceiling_new)";
    uint32_t before = ray_sym_count();
    r = ray_read_value(t2, strlen(t2), &l);
    TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    TEST_ASSERT_EQ_U(ray_sym_count(), before);
    /* ...but a document of known names still reads */
    const char* t3 = "(list 'fresh_x)";
    r = ray_read_value(t3, strlen(t3), &l);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    ray_release(r);
    PASS();
}

static test_result_t large_values(void) {
    /* a long vector, a long string, and an escape-heavy string whose decoded
     * form is a fraction of its text */
    size_t n = 200000;
    char* t = malloc(n * 8 + 64);
    size_t o = (size_t)sprintf(t, "[");
    for (size_t i = 0; i < n; i++) o += (size_t)sprintf(t + o, "%zu ", i);
    o += (size_t)sprintf(t + o, "]");
    ray_t* r = ray_read_value(t, o, NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(r->len, (int64_t)n);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(r))[n - 1], (int64_t)n - 1);
    ray_release(r);
    free(t);

    size_t sn = 1u << 20;
    t = malloc(sn * 4 + 8);
    o = 0;
    t[o++] = '"';
    for (size_t i = 0; i < sn; i++) { t[o++] = '\\'; t[o++] = 'n'; }
    t[o++] = '"';
    r = ray_read_value(t, o, NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_U(ray_str_len(r), sn);
    TEST_ASSERT_EQ_I(ray_str_ptr(r)[sn - 1], '\n');
    ray_release(r);
    free(t);
    PASS();
}

static test_result_t deep_nesting_is_bounded(void) {
    /* 100000 nested lists must be refused by the depth limit, not recursed */
    size_t n = 100000;
    char* t = malloc(n * 7 + 16);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) { memcpy(t + o, "(list ", 6); o += 6; }
    t[o++] = '1';
    for (size_t i = 0; i < n; i++) t[o++] = ')';
    ray_t* r = ray_read_value(t, o, NULL);
    TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    ray_read_limits_t l = ray_read_limits_default();
    l.max_depth = 1000000;   /* a caller cannot lift the recursion cap */
    r = ray_read_value(t, o, &l);
    TEST_ASSERT_TRUE(is_err(r, "limit")); drop(r);
    free(t);
    PASS();
}


/* ---- randomized round trip with an independent oracle ---------------------
 * Values are built with the C constructors, written by repr, read back and
 * compared on type, shape and stored bytes (symbols by their text, since ids
 * are not identity).  Printed output is never the oracle. */

static uint64_t rng_state;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}
static size_t below(size_t n) { return (size_t)(rnd() % n); }

static size_t rtext(char* out, size_t cap, bool symbolic) {
    static const char alpha[] = "abcXYZ019 _.-'\"\\;()[]{}\n\t,:#";
    size_t n = below(cap);
    for (size_t i = 0; i < n; i++) {
        if (symbolic) out[i] = alpha[below(sizeof alpha - 1)];
        else out[i] = below(8) == 0 ? (char)below(256) : alpha[below(sizeof alpha - 1)];
    }
    return n;
}

static ray_t* rvec(int8_t t, size_t n) {
    ray_t* v;
    if (t == RAY_SYM) {
        v = ray_sym_vec_new(RAY_SYM_W64, (int64_t)n);
        for (size_t i = 0; i < n; i++) {
            char buf[24];
            size_t k = rtext(buf, sizeof buf, true);
            int64_t id = k ? ray_sym_intern(buf, k) : 0;
            v = ray_vec_append(v, &id);
        }
        return v;
    }
    if (t == RAY_STR) {
        v = ray_vec_new(RAY_STR, (int64_t)n);
        for (size_t i = 0; i < n; i++) {
            char buf[40];
            size_t k = below(5) == 0 ? 0 : rtext(buf, sizeof buf, false);
            v = ray_str_vec_append(v, buf, k);
        }
        return v;
    }
    v = ray_vec_new(t, (int64_t)n);
    v->len = (int64_t)n;
    uint8_t* d = ray_data(v);
    for (size_t i = 0; i < n; i++) {
        switch (t) {
        case RAY_BOOL: case RAY_U8: d[i] = (uint8_t)rnd(); if (t == RAY_BOOL) d[i] &= 1; break;
        case RAY_I16: ((int16_t*)d)[i] = (int16_t)(rnd() % 65535 - 32767); break;
        case RAY_I32: ((int32_t*)d)[i] = (int32_t)((int64_t)(rnd() % 0xFFFFFFFFull) - 2147483647); break;
        case RAY_I64: ((int64_t*)d)[i] = (int64_t)rnd(); if (((int64_t*)d)[i] == INT64_MIN) ((int64_t*)d)[i] = 1; break;
        case RAY_F32: { float f; do { uint32_t b = (uint32_t)rnd(); memcpy(&f, &b, 4); } while (!isfinite(f)); ((float*)d)[i] = f; break; }
        case RAY_F64: { double f; do { uint64_t b = rnd(); memcpy(&f, &b, 8); } while (!isfinite(f)); ((double*)d)[i] = f; break; }
        case RAY_DATE: ((int32_t*)d)[i] = (int32_t)below(2000000) - 600000; break;      /* years ~300 to ~7400 */
        case RAY_TIME: ((int32_t*)d)[i] = (int32_t)below(86400000); break;
        case RAY_TIMESTAMP: ((int64_t*)d)[i] = (int64_t)(rnd() % (2ull << 60)) - (1ll << 60); break;
        case RAY_TIMESPAN: ((int64_t*)d)[i] = (int64_t)rnd(); if (((int64_t*)d)[i] == INT64_MIN) ((int64_t*)d)[i] = 5; break;
        case RAY_GUID: for (int k = 0; k < 16; k++) d[i * 16 + k] = (uint8_t)(rnd() | 1); break;
        }
    }
    if (t != RAY_BOOL && t != RAY_U8 && n)
        for (size_t i = 0; i < n; i++)
            if (below(7) == 0) ray_vec_set_null(v, (int64_t)i, true);
    return v;
}

static ray_t* rvalue(int depth) {
    static const int8_t vt[] = { RAY_BOOL, RAY_U8, RAY_I16, RAY_I32, RAY_I64, RAY_F32, RAY_F64, RAY_DATE,
                                 RAY_TIME, RAY_TIMESTAMP, RAY_TIMESPAN, RAY_GUID, RAY_SYM, RAY_STR };
    size_t k = below(depth > 0 ? 6 : 3);
    if (k <= 1) return rvec(vt[below(sizeof vt)], below(5) == 0 ? 0 : 1 + below(6));
    if (k == 2) {
        switch (below(6)) {
        case 0: return ray_i64((int64_t)rnd() | 1);
        case 1: return ray_f64(below(2) ? -0.0 : 1.0 / (double)(1 + below(1000)));
        case 2: { char b[30]; size_t n = rtext(b, sizeof b, false); return ray_str(b, n); }
        case 3: return ray_bool(below(2));
        case 4: return ray_i16((int16_t)below(1000));
        default: return RAY_NULL_OBJ;
        }
    }
    if (k == 3) {
        ray_t* l = ray_list_new(4);
        size_t n = below(4);
        for (size_t i = 0; i < n; i++) { ray_t* c = rvalue(depth - 1); l = ray_list_append(l, c); if (c != RAY_NULL_OBJ) ray_release(c); }
        return l;
    }
    size_t n = below(4);
    if (k == 4) {
        ray_t* keys = rvec(below(2) ? RAY_SYM : RAY_I64, n);
        ray_t* vals = ray_list_new(4);
        for (size_t i = 0; i < n; i++) { ray_t* c = rvalue(depth - 1); vals = ray_list_append(vals, c); if (c != RAY_NULL_OBJ) ray_release(c); }
        return ray_dict_new(keys, vals);
    }
    ray_t* t = ray_table_new((int64_t)n);
    size_t rows = below(5);
    for (size_t i = 0; i < n; i++) {
        char nm[8];
        int len = snprintf(nm, sizeof nm, "c%zu", i);
        ray_t* col;
        if (below(4) == 0) {
            col = ray_list_new(4);
            for (size_t r = 0; r < rows; r++) { ray_t* c = ray_i64((int64_t)r); col = ray_list_append(col, c); ray_release(c); }
        } else col = rvec(vt[below(sizeof vt)], rows);
        t = ray_table_add_col(t, ray_sym_intern(nm, (size_t)len), col);
        ray_release(col);
    }
    return t;
}

static bool same_text(ray_t* a, ray_t* b) {
    return ray_str_len(a) == ray_str_len(b) && memcmp(ray_str_ptr(a), ray_str_ptr(b), ray_str_len(a)) == 0;
}

static bool identical(ray_t* a, ray_t* b) {
    if (a == RAY_NULL_OBJ || b == RAY_NULL_OBJ) return a == b;
    if (a->type != b->type) return false;
    int8_t t = a->type;
    if (t < 0) {
        switch (t) {
        case -RAY_STR: return same_text(a, b);
        case -RAY_GUID: return memcmp(ray_data(a->obj), ray_data(b->obj), 16) == 0;
        case -RAY_F64: case -RAY_F32: return memcmp(&a->f64, &b->f64, 8) == 0;
        case -RAY_BOOL: case -RAY_U8: return a->u8 == b->u8;
        case -RAY_I16: return a->i16 == b->i16;
        case -RAY_SYM: return a->i64 == b->i64;
        case -RAY_I32: case -RAY_DATE: case -RAY_TIME: return a->i32 == b->i32;
        default: return a->i64 == b->i64;
        }
    }
    if (a->len != b->len) return false;
    if (t == RAY_LIST) {
        for (int64_t i = 0; i < a->len; i++)
            if (!identical(((ray_t**)ray_data(a))[i], ((ray_t**)ray_data(b))[i])) return false;
        return true;
    }
    if (t == RAY_DICT) return identical(ray_dict_keys(a), ray_dict_keys(b)) && identical(ray_dict_vals(a), ray_dict_vals(b));
    if (t == RAY_TABLE) {
        if (ray_table_ncols(a) != ray_table_ncols(b)) return false;
        for (int64_t i = 0; i < ray_table_ncols(a); i++) {
            if (ray_table_col_name(a, i) != ray_table_col_name(b, i)) return false;
            if (!identical(ray_table_get_col_idx(a, i), ray_table_get_col_idx(b, i))) return false;
        }
        return true;
    }
    if (t == RAY_SYM) {
        for (int64_t i = 0; i < a->len; i++) {
            ray_t* x = ray_sym_vec_cell(a, i); ray_t* y = ray_sym_vec_cell(b, i);
            if (!same_text(x, y)) return false;
        }
        return true;
    }
    if (t == RAY_STR) {
        for (int64_t i = 0; i < a->len; i++) {
            size_t la, lb;
            const char* x = ray_str_vec_get(a, i, &la); const char* y = ray_str_vec_get(b, i, &lb);
            if (la != lb || (la && memcmp(x, y, la))) return false;
        }
        return true;
    }
    size_t es = ray_type_sizes[(uint8_t)t];
    return memcmp(ray_data(a), ray_data(b), (size_t)a->len * es) == 0;
}

static test_result_t random_round_trip(void) {
    size_t bad = 0, total = 0;
    for (uint64_t seed = 1; seed <= 4000; seed++) {
        rng_state = seed * 0x9E3779B97F4A7C15ull | 1;
        ray_t* v = rvalue(3);
        ray_t* text = ray_fmt_limited(v, RAY_FMT_ROUNDTRIP, 1 << 20);
        if (RAY_IS_ERR(text)) { if (getenv("RV_DEBUG")) fprintf(stderr, "skip seed %llu: %s %s\n", (unsigned long long)seed, ray_err_code(text), ray_error_msg()); ray_error_free(text); ray_release(v); continue; }
        ray_t* back = ray_read_value(ray_str_ptr(text), ray_str_len(text), NULL);
        total++;
        if (RAY_IS_ERR(back)) {
            if (bad++ < 3) fprintf(stderr, "seed %llu: %s: %s\n  text: %.200s\n", (unsigned long long)seed,
                                   ray_err_code(back), ray_error_msg(), ray_str_ptr(text));
            ray_error_free(back);
        } else {
            if (!identical(v, back)) {
                if (bad++ < 3) fprintf(stderr, "seed %llu: not identical\n  text: %.200s\n", (unsigned long long)seed, ray_str_ptr(text));
            }
            ray_release(back);
        }
        ray_release(text);
        ray_release(v);
    }
    TEST_ASSERT(total > 3000, "most generated values are writable");
    TEST_ASSERT_EQ_U(bad, 0);
    PASS();
}

static test_result_t reads_after_a_persisted_sym_load(void) {
    char path[512];
    snprintf(path, sizeof path, "%s/readval.sym", ray_test_tmp_root());
    int64_t keep = ray_sym_intern("persisted_a", 11);
    for (int i = 0; i < 300; i++) {   /* a file big enough to load lazily where the threshold is low */
        char nm[32];
        int n = snprintf(nm, sizeof nm, "persisted_filler_%d", i);
        (void)ray_sym_intern(nm, (size_t)n);
    }
    TEST_ASSERT_EQ_I(ray_sym_save(path), RAY_OK);
    ray_sym_destroy();
    (void)ray_sym_init();
    TEST_ASSERT_EQ_I(ray_sym_load(path), RAY_OK);
    const char* t = "(list 'persisted_a 'persisted_b)";
    uint32_t before = ray_sym_count();
    ray_t* r = ray_read_value(t, strlen(t), NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(r));
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(r))[0]->i64, keep);
    TEST_ASSERT_EQ_I(((ray_t**)ray_data(r))[1]->i64, ray_sym_find("persisted_b", 11));
    TEST_ASSERT_EQ_U(ray_sym_count(), before + 1);
    ray_release(r);
    PASS();
}

/* The data-node budget counts occurrences in the VALUE, not in the syntax that
 * wrote it: every scalar, every vector element and every container (a vector,
 * a list, a dict, a table) counts one.  node_count() states that rule over the
 * returned value, independently of the reader's own counter, and each text is
 * read at exactly that cap (succeeds) and one below it (a limit error).  Two
 * spellings of the same value must therefore cost the same. */
static size_t node_count(ray_t* v) {
    if (!v || v->type < 0 || v->type == RAY_NULL) return 1;            /* a scalar, null */
    if (v->type == RAY_LIST) {
        size_t n = 1;
        for (int64_t i = 0; i < v->len; i++) n += node_count(((ray_t**)ray_data(v))[i]);
        return n;
    }
    if (v->type == RAY_DICT) return 1 + node_count(ray_dict_keys(v)) + node_count(ray_dict_vals(v));
    if (v->type == RAY_TABLE) {
        ray_t** slots = (ray_t**)ray_data(v);            /* the schema, then the columns */
        return 1 + ((size_t)slots[0]->len + 1) + node_count(slots[1]);
    }
    return (size_t)v->len + 1;                                          /* a typed vector */
}

static size_t node_cost(const char* text) {
    ray_read_limits_t l = ray_read_limits_default();
    ray_t* r = ray_read_value(text, strlen(text), &l);
    if (RAY_IS_ERR(r)) { drop(r); return 0; }
    size_t want = node_count(r);
    ray_release(r);
    return want;
}

static bool reads_within(const char* text, size_t cap) {
    ray_read_limits_t l = ray_read_limits_default();
    l.max_nodes = cap;
    ray_t* r = ray_read_value(text, strlen(text), &l);
    bool ok = !RAY_IS_ERR(r);
    if (!ok && !is_err(r, "limit")) { drop(r); return true; }   /* a wrong error kind fails the caller's assert */
    drop(r);
    return ok;
}

static test_result_t node_count_contract(void) {
    static const struct { const char* text; size_t nodes; } cases[] = {
        { "1", 1 }, { "null", 1 }, { "\"a string well past the inline limit\"", 1 }, { "'one", 1 },
        { "[1 2 3]", 4 }, { "(as 'I64 [])", 1 }, { "['a 'b]", 3 }, { "[\"\" \"x\"]", 3 },
        { "(as 'SYM (list \"x\" \"\"))", 3 },
        { "(as 'GUID \"0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\")", 1 },
        { "()", 1 }, { "(list)", 1 }, { "(list 1 2)", 3 }, { "(list [1 2] (list 3))", 6 },
        /* the two spellings of one dictionary cost the same: keys 3, values 3, the dict 1 */
        { "(dict ['a 'b] (list 1 2))", 7 }, { "{a: 1 b: 2}", 7 },
        { "(dict [\"a\" \"b\"] (list 1 2))", 7 }, { "{\"a\": 1 \"b\": 2}", 7 },
        { "(dict (list) (list))", 3 }, { "{}", 3 },
        /* keys 3, values list 1 + [1 2] 3 + (list 1) 2, the dict 1 */
        { "{a: [1 2] b: (list 1)}", 10 }, { "(dict ['a 'b] (list [1 2] (list 1)))", 10 },
        /* inner dict 5 (keys 2, values 2, dict 1); outer: keys 2, values list 1 + 5, dict 1 */
        { "{a: {b: 1}}", 9 }, { "(dict ['a] (list (dict ['b] (list 1))))", 9 },
        /* the table 1, its names 3, its columns: list 1 + [1 2] 3 + (list 1 "x") 3 */
        { "(table (as 'SYM (list \"c1\" \"c2\")) (list [1 2] (list 1 \"x\")))", 11 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        const char* t = cases[i].text;
        size_t want = cases[i].nodes;
        char msg[160];
        snprintf(msg, sizeof msg, "node cost of %.100s", t);
        TEST_ASSERT(node_cost(t) == want, msg);   /* the table is the rule applied to the value */
        snprintf(msg, sizeof msg, "reads at exactly its cost: %.100s", t);
        TEST_ASSERT(reads_within(t, want), msg);
        snprintf(msg, sizeof msg, "refused one node below its cost: %.100s", t);
        TEST_ASSERT(!reads_within(t, want - 1), msg);
    }
    PASS();
}

const test_entry_t readval_entries[] = {
    {"readval/limit_edges", limit_edges, setup, teardown},
    {"readval/node_count_contract", node_count_contract, setup, teardown},
    {"readval/faults_leave_nothing", faults_leave_nothing, setup, teardown},
    {"readval/release_returns_everything", release_returns_everything, setup, teardown},
    {"readval/rejected_reads_add_no_symbols", rejected_reads_add_no_symbols, setup, teardown},
    {"readval/symbols_commit_once_complete", symbols_commit_once_complete, setup, teardown},
    {"readval/large_values", large_values, setup, teardown},
    {"readval/deep_nesting_is_bounded", deep_nesting_is_bounded, setup, teardown},
    {"readval/random_round_trip", random_round_trip, setup, teardown},
    {"readval/reads_after_a_persisted_sym_load", reads_after_a_persisted_sym_load, setup, teardown},
    {NULL, NULL, NULL, NULL}
};
