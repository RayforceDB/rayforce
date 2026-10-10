/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L  /* setenv / unsetenv */
#endif
#include "test.h"
#include "io/parquet.h"
#include "core/pool.h"
#include "mem/heap.h"
#include "mem/sys.h"
#include "table/sym.h"
#include "store/part.h"
#include "store/splay.h"
#include "ops/idxop.h"
#include "core/runtime.h"   /* ray_error_msg: the skeptic probes */
#include "core/profile.h"   /* ray_profile_now_ns: pq_pool's bounded wait */
#include "core/crc32.h"     /* ray_crc32: page checksums of the made files */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
#include <pthread.h>
#include <sys/wait.h>   /* the skeptic probes' child imports */
#endif

static void pq_setup(void) { ray_heap_init(); (void)ray_sym_init(); }
static void pq_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); }
/* A pool of `cores` workers, every one of them started.  Its workers read
 * the environment as they start (ray_heap_init's getenv), and these tests
 * set and unset variables right after making a pool; ray_pool_init_total
 * waits for the workers itself, and this does not rely on it.  A worker
 * whose heap never shows (its ray_heap_init failed) fails the pool within
 * a bound instead of hanging the test. */
static ray_err_t pq_pool(int cores) {
    ray_pool_destroy();
    ray_err_t e = ray_pool_init_total((uint32_t)cores);
    ray_pool_t* p = ray_pool_get();
    int64_t until = ray_profile_now_ns() + (int64_t)30 * 1000000000;
    for (uint32_t i = 0; e == RAY_OK && p && i < p->n_workers; i++)
        while (!atomic_load_explicit(&p->worker_heaps[i], memory_order_acquire)) {
            if (ray_profile_now_ns() > until) {
                fprintf(stderr, "  pq_pool: worker %u of %u has no heap\n", i, p->n_workers);
                return RAY_ERR_OOM;
            }
            RAY_CPU_RELAX();
        }
    return e;
}
#define FIX "test/data/parquet/"

static test_result_t test_pq_snappy(void) {
    uint8_t dst[256];
    /* Literal 'ab' followed by overlapping copies: all three copy tags. */
    const uint8_t b[] = {20,4,'a','b',1,2,14,2,0,39,2,0,0,0};
    TEST_ASSERT_TRUE(ray_parquet_snappy(b,sizeof(b),dst,20));
    for (int i = 0; i < 20; i++) TEST_ASSERT_EQ_I(dst[i],i&1 ? 'b' : 'a');
    for (size_t i = 0; i < sizeof(b); i++) TEST_ASSERT_FALSE(ray_parquet_snappy(b,i,dst,20));
    TEST_ASSERT_FALSE(ray_parquet_snappy(b,sizeof(b),dst,19));
    uint8_t invalid[] = {4,1,0};
    TEST_ASSERT_FALSE(ray_parquet_snappy(invalid,sizeof(invalid),dst,4));
    invalid[2] = 3;
    TEST_ASSERT_FALSE(ray_parquet_snappy(invalid,sizeof(invalid),dst,4));
    const uint8_t empty[] = {0};
    TEST_ASSERT_TRUE(ray_parquet_snappy(empty,1,dst,0));
    /* Exhaust every offset for a 128-byte prefix and all COPY_2 lengths.
     * Includes disjoint and overlapping copies with nonperiodic prefixes. */
    for (int off = 1; off <= 128; off++) for (int n = 1; n <= 64; n++) {
        uint8_t encoded[140], expected[192]; size_t at = 0;
        encoded[at++] = (uint8_t)((128+n)|128); encoded[at++] = 1;
        encoded[at++] = 240; encoded[at++] = 127;
        for (int i = 0; i < 128; i++) encoded[at++] = expected[i] = (uint8_t)(i*31+i/7);
        encoded[at++] = (uint8_t)(((n-1)<<2)|2); encoded[at++] = (uint8_t)off; encoded[at++] = 0;
        for (int i = 0; i < n; i++) expected[128+i] = expected[128+i-off];
        TEST_ASSERT_TRUE(ray_parquet_snappy(encoded,at,dst,128+n));
        TEST_ASSERT_TRUE(!memcmp(expected,dst,128+n));
    }
    PASS();
}
static test_result_t test_pq_matrix(void) {
    const char* names[] = {FIX "flat-0-v1.parquet",FIX "flat-0-v2.parquet",FIX "flat-1-v1.parquet",FIX "flat-1-v2.parquet",FIX "legacy.parquet",FIX "v2-uncompressed.parquet",FIX "no-statistics.parquet"};
    int64_t expect[] = {1,NULL_I32,3,4,5,NULL_I32,7,8,9};
    const char* strings[] = {"alpha","a long string over twelve bytes","","","alpha","beta","gamma","delta","z"};
    for (int file = 0; file < 7; file++) for (int batch = 1; batch <= 11; batch++) {
        ray_parquet_t* r = NULL;
        ray_t* err = ray_parquet_open(names[file],NULL,batch,&r);
        TEST_ASSERT_NULL(err); TEST_ASSERT_NOT_NULL(r);
        int64_t total = 0; ray_t* kept = NULL;
        for (;;) {
            ray_t* t = ray_parquet_next(r);
            if (!t) break;
            TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_ncols(t),7);
            int64_t nr = ray_table_nrows(t);
            TEST_ASSERT_TRUE(nr <= batch);
            for (int64_t row = 0; row < nr; row++) {
                int j = (int)((total+row)%9);
                ray_t* x = ray_table_get_col_idx(t,0);
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(x))[row],expect[j]);
                size_t len; const char* str = ray_str_vec_get(ray_table_get_col_idx(t,1),row,&len);
                TEST_ASSERT_EQ_I(len,strlen(strings[j]));
                TEST_ASSERT_TRUE(memcmp(str,strings[j],len) == 0);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(t,3)))[row],100+j);
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(t,4)))[row],j);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(t,5)))[row],j*1000);
            }
            total += nr;
            if (!kept) kept = t; else ray_release(t);
        }
        TEST_ASSERT_EQ_I(total,18);
        ray_parquet_close(r);
        /* Results retain no pointers into the reader's mapping or scratch. */
        size_t len; const char* str = ray_str_vec_get(ray_table_get_col_idx(kept,1),0,&len);
        TEST_ASSERT_EQ_I(len,5); TEST_ASSERT_TRUE(memcmp(str,"alpha",5) == 0);
        ray_release(kept);
    }
    PASS();
}
static test_result_t test_pq_read_meta(void) {
    ray_t* m = ray_parquet_metadata(FIX "flat-1-v1.parquet");
    TEST_ASSERT_FALSE(RAY_IS_ERR(m)); TEST_ASSERT_EQ_I(ray_table_nrows(m),14);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(m,4)))[0],1);
    ray_release(m);
    int64_t names[] = {ray_sym_intern("name",4),ray_sym_intern("x",1)};
    ray_t* cols = ray_vec_from_raw(RAY_SYM,names,2);
    ray_t* t = ray_parquet_read(FIX "flat-1-v2.parquet",cols);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_ncols(t),2); TEST_ASSERT_EQ_I(ray_table_nrows(t),18);
    TEST_ASSERT_EQ_I(ray_table_col_name(t,0),names[0]);
    TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(t,1)))[17],9);
    ray_release(t); ray_release(cols);
    t = ray_parquet_read(FIX "empty.parquet",NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_nrows(t),0); TEST_ASSERT_EQ_I(ray_table_ncols(t),7); ray_release(t);
    t = ray_parquet_read(FIX "all-null.parquet",NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_nrows(t),9);
    for (int c = 0; c < 2; c++) for (int i = 0; i < 9; i++) TEST_ASSERT_TRUE(ray_vec_is_null(ray_table_get_col_idx(t,c),i));
    ray_release(t);
    t = ray_parquet_read(FIX "sentinels.parquet",NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    for (int c = 0; c < 4; c++) TEST_ASSERT_TRUE(ray_vec_is_null(ray_table_get_col_idx(t,c),0));
    ray_release(t);
    t = ray_parquet_read(FIX "bool.parquet",NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    for (int i = 0; i < 10; i++) TEST_ASSERT_EQ_I(((uint8_t*)ray_data(ray_table_get_col_idx(t,0)))[i],!(i&1));
    ray_release(t);
    PASS();
}
static test_result_t test_pq_corrupt(void) {
    FILE* f = fopen(FIX "flat-1-v2.parquet","rb"); TEST_ASSERT_NOT_NULL(f);
    uint8_t bytes[16384]; size_t n = fread(bytes,1,sizeof(bytes),f); fclose(f);
    TEST_ASSERT_TRUE(n < sizeof(bytes));
    char path[100]; snprintf(path,sizeof(path),"/tmp/rayforce-pq-corrupt-%d.parquet",(int)getpid());
    /* Every truncation must fail without reading outside the mapped file. */
    for (size_t i = 0; i < n; i++) {
        f = fopen(path,"wb"); TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_EQ_I(fwrite(bytes,1,i,f),i); fclose(f);
        ray_t* err = ray_parquet_read(path,NULL);
        TEST_ASSERT_TRUE(RAY_IS_ERR(err)); ray_release(err);
    }
    /* Corrupt each byte in the first page: checksum/header failures propagate. */
    for (size_t i = 4; i < 30; i++) {
        bytes[i] ^= 0x80;
        f = fopen(path,"wb"); TEST_ASSERT_NOT_NULL(f); TEST_ASSERT_EQ_I(fwrite(bytes,1,n,f),n); fclose(f);
        ray_t* err = ray_parquet_read(path,NULL);
        TEST_ASSERT_TRUE(RAY_IS_ERR(err)); ray_release(err); bytes[i] ^= 0x80;
    }
    unlink(path);
    int64_t badname = ray_sym_intern("missing",7);
    ray_t* cols = ray_vec_from_raw(RAY_SYM,&badname,1); ray_parquet_t* r = NULL;
    ray_t* err = ray_parquet_open(FIX "flat-1-v1.parquet",cols,16,&r);
    TEST_ASSERT_TRUE(RAY_IS_ERR(err)); TEST_ASSERT_NULL(r); ray_release(err); ray_release(cols);
    PASS();
}
static test_result_t test_pq_native(void) {
    char root[128]; snprintf(root,sizeof(root),"/tmp/rayforce-pq-native-%d",(int)getpid());
    ray_t* n = ray_parquet_parted(FIX "flat-1-v2.parquet",root,"hits");
    TEST_ASSERT_FALSE(RAY_IS_ERR(n)); TEST_ASSERT_EQ_I(n->i64,18); ray_release(n);
    ray_t* t = ray_read_parted(root,"hits");
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_nrows(t),18);
    ray_t* x = ray_table_get_col(t,ray_sym_intern("x",1));
    ray_t* seg = ((ray_t**)ray_data(x))[0];
    TEST_ASSERT_TRUE(ray_index_kind(seg) != 0);
    ray_release(t);
    /* Never overwrite an existing native destination. */
    n = ray_parquet_parted(FIX "flat-1-v2.parquet",root,"hits");
    TEST_ASSERT_TRUE(RAY_IS_ERR(n)); ray_release(n);
    const char* names[] = {"x","name","flag","wide","day","ts","f",".d"};
    for (int p = 0; p < 2; p++) {
        char path[200];
        for (int c = 0; c < 8; c++) { snprintf(path,sizeof(path),"%s/%d/hits/%s",root,p,names[c]); unlink(path); }
        snprintf(path,sizeof(path),"%s/%d/hits",root,p); rmdir(path);
        snprintf(path,sizeof(path),"%s/%d",root,p); rmdir(path);
    }
    rmdir(root);
    PASS();
}
static test_result_t test_pq_range(void) {
    int64_t name = ray_sym_intern("name",4), x = ray_sym_intern("x",1);
    ray_t* cols = ray_vec_from_raw(RAY_SYM,&name,1);
    for (int pass = 0; pass < 3; pass++) {
        ray_parquet_t* r = NULL;
        ray_t* err = ray_parquet_open(pass == 2 ? FIX "no-statistics.parquet" : FIX "flat-1-v2.parquet",cols,2,&r);
        TEST_ASSERT_NULL(err);
        err = ray_parquet_range(r,x,pass ? 40 : 4,pass ? 70 : 7);
        TEST_ASSERT_NULL(err);
        int64_t count = 0;
        for (;;) {
            ray_t* t = ray_parquet_next(r);
            if (!t) break;
            TEST_ASSERT_FALSE(RAY_IS_ERR(t));
            TEST_ASSERT_EQ_I(ray_table_ncols(t),1);
            count += ray_table_nrows(t); ray_release(t);
        }
        TEST_ASSERT_EQ_I(count,pass ? 0 : 6);
        TEST_ASSERT_EQ_I(ray_parquet_groups_skipped(r),pass == 1 ? 2 : 0);
        ray_parquet_close(r);
    }
    ray_release(cols);
    PASS();
}
static test_result_t test_pq_indexes(void) {
    int64_t names[] = {ray_sym_intern("name",4),ray_sym_intern("wide",4)};
    int64_t x = ray_sym_intern("x",1);
    ray_t* cols = ray_vec_from_raw(RAY_SYM,names,2);
    const char* files[] = {"indexed-v1.parquet","indexed-v2.parquet","indexed-v1-bad.parquet","indexed-v2-bad.parquet"};
    for (int file = 0; file < 4; file++) for (int batch = 1; batch <= 11; batch++) {
        char path[128]; snprintf(path,sizeof(path),FIX "%s",files[file]);
        ray_parquet_t* r = NULL;
        TEST_ASSERT_NULL(ray_parquet_open(path,cols,batch,&r));
        TEST_ASSERT_NULL(ray_parquet_range(r,x,4,5));
        int64_t count = 0;
        for (;;) {
            ray_t* t = ray_parquet_next(r); if (!t) break;
            TEST_ASSERT_FALSE(RAY_IS_ERR(t));
            ray_t* wide = ray_table_get_col_idx(t,1);
            for (int64_t row = 0; row < wide->len; row++,count++) {
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(wide))[row],103+count%2);
                size_t n; const char* p = ray_str_vec_get(ray_table_get_col_idx(t,0),row,&n);
                TEST_ASSERT_EQ_I(n,count%2 ? 5 : 0);
                if (n) TEST_ASSERT_TRUE(!memcmp(p,"alpha",5));
            }
            ray_release(t);
        }
        TEST_ASSERT_EQ_I(count,4);
        TEST_ASSERT_EQ_I(ray_parquet_groups_skipped(r),0);
        if (file < 2) TEST_ASSERT_TRUE(ray_parquet_pages_skipped(r) > 0);
        else TEST_ASSERT_EQ_I(ray_parquet_pages_skipped(r),0);
        ray_parquet_close(r);
    }
    ray_release(cols);
    ray_parquet_t* r = NULL;
    TEST_ASSERT_NULL(ray_parquet_open(FIX "indexed-null.parquet",NULL,2,&r));
    TEST_ASSERT_NULL(ray_parquet_range(r,x,0,10));
    int64_t count = 0;
    for (;;) {
        ray_t* t = ray_parquet_next(r); if (!t) break;
        TEST_ASSERT_FALSE(RAY_IS_ERR(t));
        ray_t* v = ray_table_get_col_idx(t,1);
        for (int64_t i = 0; i < v->len; i++,count++) TEST_ASSERT_EQ_I(((int64_t*)ray_data(v))[i],3+count);
        ray_release(t);
    }
    TEST_ASSERT_EQ_I(count,3); TEST_ASSERT_TRUE(ray_parquet_pages_skipped(r) >= 2);
    ray_parquet_close(r);
    PASS();
}
static test_result_t test_pq_bloom(void) {
    int64_t x = ray_sym_intern("x",1), wide = ray_sym_intern("wide",4);
    for (int bad = 0; bad < 2; bad++) for (int col = 0; col < 2; col++) for (int val = 1; val <= 9; val++) {
        ray_parquet_t* r = NULL;
        TEST_ASSERT_NULL(ray_parquet_open(bad ? FIX "bloom-bad.parquet" : FIX "bloom.parquet",NULL,2,&r));
        int64_t target = col ? 99+val : val;
        TEST_ASSERT_NULL(ray_parquet_range(r,col ? wide : x,target,target));
        int64_t count = 0;
        for (;;) {
            ray_t* t = ray_parquet_next(r); if (!t) break;
            TEST_ASSERT_FALSE(RAY_IS_ERR(t)); count += ray_table_nrows(t); ray_release(t);
        }
        bool absent = !col && (val == 2 || val == 6);
        TEST_ASSERT_EQ_I(count,absent ? 0 : 2);
        TEST_ASSERT_EQ_I(ray_parquet_bloom_skipped(r),!bad && absent ? 2 : 0);
        ray_parquet_close(r);
    }
    PASS();
}
static test_result_t test_pq_parallel(void) {
    ray_t* sentinels = ray_parquet_read(FIX "simd-sentinels.parquet",NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(sentinels));
    for (int col = 0; col < 2; col++) {
        ray_t* v = ray_table_get_col_idx(sentinels,col);
        TEST_ASSERT_TRUE(v->attrs & RAY_ATTR_HAS_NULLS);
        for (int i = 0; i < 39; i++) TEST_ASSERT_EQ_I(ray_vec_is_null(v,i),i%(col ? 5 : 7) == 0);
    }
    ray_release(sentinels);
    for (int workers = 1; workers <= 3; workers += 2) {
        TEST_ASSERT_EQ_I(pq_pool(workers), RAY_OK);
        ray_parquet_t* r = NULL;
        TEST_ASSERT_NULL(ray_parquet_open(FIX "parallel.parquet",NULL,8192,&r));
        int64_t count = 0; ray_t* retained = NULL;
        for (;;) {
            ray_t* t = ray_parquet_next(r); if (!t) break;
            TEST_ASSERT_FALSE(RAY_IS_ERR(t));
            ray_t* x = ray_table_get_col_idx(t,0); ray_t* y = ray_table_get_col_idx(t,1);
            for (int64_t i = 0; i < x->len; i++,count++) {
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(x))[i],count);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(y))[i],count*INT64_C(10000000001));
                char expect[10]; int n = snprintf(expect,sizeof(expect),"s%lld",(long long)(count%11));
                size_t len; const char* p = ray_str_vec_get(ray_table_get_col_idx(t,2),i,&len);
                TEST_ASSERT_EQ_I(len,n); TEST_ASSERT_TRUE(!memcmp(p,expect,len));
            }
            if (!retained) retained = t; else ray_release(t);
        }
        TEST_ASSERT_EQ_I(count,70003);
        if (workers > 1) TEST_ASSERT_TRUE(ray_parquet_parallel_batches(r) > 0);
        else TEST_ASSERT_EQ_I(ray_parquet_parallel_batches(r),0);
        ray_parquet_close(r);
        TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(retained,0)))[100],100);
        ray_release(retained);
        ray_pool_destroy();
    }
    PASS();
}
static void pq_remove_native(const char* dir, const char** columns, int n) {
    char path[512];
    for (int i = 0; i < n; i++) { snprintf(path,sizeof(path),"%s/%s",dir,columns[i]); unlink(path); }
    const char* special[] = {".d",".sym",".sym.lk"};
    for (int i = 0; i < 3; i++) { snprintf(path,sizeof(path),"%s/%s",dir,special[i]); unlink(path); }
    rmdir(dir);
}
/* The direct import picks its pass layout from memory; RAY_PQ_PASS_COLS
 * forces it, so a test covers both: NULL = the default (one pass on any
 * test machine), "1" = column by column. */
static void pq_set_env(const char* name, const char* v) {
#ifdef RAY_OS_WINDOWS
    _putenv_s(name, v ? v : "");
#else
    if (v) setenv(name, v, 1); else unsetenv(name);
#endif
}
static void pq_set_layout(const char* v) { pq_set_env("RAY_PQ_PASS_COLS", v); }
/* The symbol columns' import: NULL = the default (direct on any test
 * file), "grouped" = two decodes and the hash-grouped dictionary
 * (table/symgrp.h), "direct" forced. */
static void pq_set_symmode(const char* v) { pq_set_env("RAY_PQ_SYM_MODE", v); }
/* Every knob of the grouped import back to its default (a failed test can
 * leave one set). */
static void pq_sym_env_clear(void) {
    const char* knobs[] = {"RAY_PQ_SYM_MODE","RAY_PQ_SYM_ORDER","RAY_PQ_SYM_GROUPS","RAY_PQ_SYM_WINDOW",
                           "RAY_PQ_SYM_ARENA","RAY_PQ_SYM_HASH_BITS","RAY_PQ_SYM_INJECT","RAY_PQ_PASS_COLS",
                           "RAY_PQ_SYM_RAM","RAY_PQ_DIRECT_REVERSE","RAY_PQ_SYM_HASH_TOP"};
    for (size_t i = 0; i < sizeof(knobs)/sizeof(knobs[0]); i++) pq_set_env(knobs[i], NULL);
}

/* A Parquet file for the grouped import's tests: `groups` row groups of
 * x, the row as a required INT32, and s, an optional BYTE_ARRAY (UTF8) that
 * fn gives (NULL for a null); PLAIN, uncompressed, one v1 data page per
 * chunk.  The thrift compact encoding of test/data/parquet/generate.py. */
typedef const char* (*pq_synth_fn)(int64_t row, char* buf, uint32_t* len);
typedef struct { uint8_t* p; size_t n, cap; bool bad; } pq_buf;
static void pb_bytes(pq_buf* b, const void* s, size_t n) {
    if (b->bad) return;
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n) cap *= 2;
        uint8_t* p = (uint8_t*)ray_sys_realloc(b->p, cap);
        if (!p) { b->bad = true; return; }
        b->p = p; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, s, n);
    b->n += n;
}
static void pb_byte(pq_buf* b, uint8_t v) { pb_bytes(b, &v, 1); }
static void pb_var(pq_buf* b, uint64_t v) {
    while (v > 127) { pb_byte(b, (uint8_t)((v & 127) | 128)); v >>= 7; }
    pb_byte(b, (uint8_t)v);
}
static void pb_zz(pq_buf* b, int64_t v) { pb_var(b, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }
enum { PB_I32 = 5, PB_I64 = 6, PB_BIN = 8, PB_LIST = 9, PB_STRUCT = 12 };
static void pb_field(pq_buf* b, int* last, int id, int type) {
    int d = id - *last;
    if (d > 0 && d < 16) pb_byte(b, (uint8_t)((d << 4) | type));
    else { pb_byte(b, (uint8_t)type); pb_zz(b, id); }
    *last = id;
}
static void pb_i(pq_buf* b, int* last, int id, int type, int64_t v) { pb_field(b, last, id, type); pb_zz(b, v); }
static void pb_bin(pq_buf* b, const char* s, size_t n) { pb_var(b, n); pb_bytes(b, s, n); }
static void pb_list(pq_buf* b, int n, int type) {
    if (n < 15) pb_byte(b, (uint8_t)((n << 4) | type));
    else { pb_byte(b, (uint8_t)(0xf0 | type)); pb_var(b, (uint64_t)n); }
}
/* ColumnMetaData 13, a chunk's encoding stats: a dictionary page, data
 * pages of dictionary ids, data pages of PLAIN values, as given. */
static void pb_enc_stats(pq_buf* f, int* last, bool dict, bool ids, bool plain) {
    pb_field(f, last, 13, PB_LIST); pb_list(f, (int)dict + (int)ids + (int)plain, PB_STRUCT);
    if (dict) { int l = 0; pb_i(f, &l, 1, PB_I32, 2); pb_i(f, &l, 2, PB_I32, 0); pb_i(f, &l, 3, PB_I32, 1); pb_byte(f, 0); }
    if (ids) { int l = 0; pb_i(f, &l, 1, PB_I32, 0); pb_i(f, &l, 2, PB_I32, 8); pb_i(f, &l, 3, PB_I32, 1); pb_byte(f, 0); }
    if (plain) { int l = 0; pb_i(f, &l, 1, PB_I32, 0); pb_i(f, &l, 2, PB_I32, 0); pb_i(f, &l, 3, PB_I32, 1); pb_byte(f, 0); }
}
/* Columns x, s (fn of the row) and t (fn of the row + rows / 2: a
 * vocabulary that meets s's, for the second column's pass).  A test may set
 * pq_synth_t, column t's own function of the row, and pq_synth_dict, the
 * columns (bit c) written dictionary-encoded (a dictionary page of the
 * chunk's distinct strings, then the ids), and resets them after. */
#define PQ_SYNTH_COLS 3
static pq_synth_fn pq_synth_t;
static unsigned pq_synth_dict;
static const char* pq_synth_row(pq_synth_fn fn, int c, int64_t r, int64_t rows, char* buf, uint32_t* len) {
    return c == 2 && pq_synth_t ? pq_synth_t(r, buf, len) : fn(c == 1 ? r : r + rows / 2, buf, len);
}
static bool pq_synth(const char* path, int64_t rows, int64_t groups, pq_synth_fn fn) {
    pq_buf f = {0}, page = {0}, levels = {0}, values = {0}, dict = {0}, doff = {0};
    int64_t* at = (int64_t*)ray_sys_alloc((size_t)groups * PQ_SYNTH_COLS * sizeof(int64_t));
    int64_t* sz = (int64_t*)ray_sys_alloc((size_t)groups * PQ_SYNTH_COLS * sizeof(int64_t));
    int64_t* dat = (int64_t*)ray_sys_alloc((size_t)groups * PQ_SYNTH_COLS * sizeof(int64_t));
    if (!at || !sz || !dat) return false;
    pb_bytes(&f, "PAR1", 4);
    char buf[256];
    for (int64_t g = 0; g < groups; g++) {
        int64_t lo = rows * g / groups, hi = rows * (g + 1) / groups;
        for (int c = 0; c < PQ_SYNTH_COLS; c++) {
            bool dicted = c && (pq_synth_dict >> c & 1);
            levels.n = values.n = page.n = dict.n = doff.n = 0;
            if (c == 0) for (int64_t r = lo; r < hi; r++) { int32_t x = (int32_t)r; pb_bytes(&values, &x, 4); }
            else {
                uint8_t bits = 0;
                int64_t n = hi - lo, groups8 = (n + 7) / 8;
                pb_var(&levels, (uint64_t)(groups8 * 2 + 1));
                pq_buf ids = {0};
                for (int64_t r = lo; r < lo + groups8 * 8; r++) {
                    uint32_t len = 0;
                    const char* s = r < hi ? pq_synth_row(fn, c, r, rows, buf, &len) : NULL;
                    if (s) {
                        bits |= (uint8_t)(1u << ((r - lo) & 7));
                        if (!dicted) { pb_bytes(&values, &len, 4); pb_bytes(&values, s, len); }
                        else {
                            /* the entry's id, the entry added on its first occurrence */
                            uint32_t id = 0, nd = (uint32_t)(doff.n / 4);
                            for (; id < nd; id++) {
                                uint32_t o; memcpy(&o, doff.p + id * 4, 4);
                                uint32_t l; memcpy(&l, dict.p + o, 4);
                                if (l == len && !memcmp(dict.p + o + 4, s, len)) break;
                            }
                            if (id == nd) { uint32_t o = (uint32_t)dict.n; pb_bytes(&doff, &o, 4); pb_bytes(&dict, &len, 4); pb_bytes(&dict, s, len); }
                            pb_bytes(&ids, &id, 4);
                        }
                    }
                    if (((r - lo) & 7) == 7) { pb_byte(&levels, bits); bits = 0; }
                }
                uint32_t ln = (uint32_t)levels.n;
                pb_bytes(&page, &ln, 4); pb_bytes(&page, levels.p, levels.n);
                if (dicted) {
                    /* the ids: their bit width, then a run of one a value */
                    uint32_t nd = (uint32_t)(doff.n / 4), bw = 1;
                    while (bw < 32 && ((uint64_t)1 << bw) < nd) bw++;
                    pb_byte(&values, (uint8_t)bw);
                    for (size_t i = 0; i + 4 <= ids.n; i += 4) {
                        uint32_t id; memcpy(&id, ids.p + i, 4);
                        pb_var(&values, 2);
                        pb_bytes(&values, &id, (bw + 7) / 8);
                    }
                    if (ids.bad) values.bad = true;
                }
                ray_sys_free(ids.p);
            }
            pb_bytes(&page, values.p, values.n);
            int last = 0;
            int64_t start = (int64_t)f.n;
            dat[g * PQ_SYNTH_COLS + c] = -1;
            if (dicted) {
                pb_i(&f, &last, 1, PB_I32, 2);
                pb_i(&f, &last, 2, PB_I32, (int64_t)dict.n);
                pb_i(&f, &last, 3, PB_I32, (int64_t)dict.n);
                pb_field(&f, &last, 7, PB_STRUCT);
                { int l = 0; pb_i(&f, &l, 1, PB_I32, (int64_t)(doff.n / 4)); pb_i(&f, &l, 2, PB_I32, 0); pb_byte(&f, 0); }
                pb_byte(&f, 0);
                pb_bytes(&f, dict.p, dict.n);
                dat[g * PQ_SYNTH_COLS + c] = (int64_t)f.n;
                last = 0;
            }
            pb_i(&f, &last, 1, PB_I32, 0);
            pb_i(&f, &last, 2, PB_I32, (int64_t)page.n);
            pb_i(&f, &last, 3, PB_I32, (int64_t)page.n);
            pb_field(&f, &last, 5, PB_STRUCT);
            int l2 = 0;
            pb_i(&f, &l2, 1, PB_I32, hi - lo); pb_i(&f, &l2, 2, PB_I32, dicted ? 8 : 0);
            pb_i(&f, &l2, 3, PB_I32, 3); pb_i(&f, &l2, 4, PB_I32, 3);
            pb_byte(&f, 0); pb_byte(&f, 0);
            pb_bytes(&f, page.p, page.n);
            at[g * PQ_SYNTH_COLS + c] = start; sz[g * PQ_SYNTH_COLS + c] = (int64_t)f.n - start;
        }
    }
    static const char* cols[PQ_SYNTH_COLS] = {"x","s","t"};
    int64_t footer = (int64_t)f.n;
    int last = 0;
    pb_i(&f, &last, 1, PB_I32, 1);
    pb_field(&f, &last, 2, PB_LIST); pb_list(&f, PQ_SYNTH_COLS + 1, PB_STRUCT);
    { int l = 0; pb_i(&f, &l, 3, PB_I32, 0); pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, "schema", 6); pb_i(&f, &l, 5, PB_I32, PQ_SYNTH_COLS); pb_byte(&f, 0); }
    { int l = 0; pb_i(&f, &l, 1, PB_I32, 1); pb_i(&f, &l, 3, PB_I32, 0); pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, "x", 1); pb_byte(&f, 0); }
    for (int c = 1; c < PQ_SYNTH_COLS; c++) {
        int l = 0;
        pb_i(&f, &l, 1, PB_I32, 6); pb_i(&f, &l, 3, PB_I32, 1);
        pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, cols[c], 1); pb_i(&f, &l, 6, PB_I32, 0); pb_byte(&f, 0);
    }
    pb_i(&f, &last, 3, PB_I64, rows);
    pb_field(&f, &last, 4, PB_LIST); pb_list(&f, (int)groups, PB_STRUCT);
    for (int64_t g = 0; g < groups; g++) {
        int64_t n = rows * (g + 1) / groups - rows * g / groups, total = 0;
        int l = 0;
        pb_field(&f, &l, 1, PB_LIST); pb_list(&f, PQ_SYNTH_COLS, PB_STRUCT);
        for (int c = 0; c < PQ_SYNTH_COLS; c++) {
            int64_t i = g * PQ_SYNTH_COLS + c;
            int lc = 0;
            pb_i(&f, &lc, 2, PB_I64, at[i]);
            pb_field(&f, &lc, 3, PB_STRUCT);
            int lm = 0;
            pb_i(&f, &lm, 1, PB_I32, c ? 6 : 1);
            if (dat[i] >= 0) { pb_field(&f, &lm, 2, PB_LIST); pb_list(&f, 3, PB_I32); pb_zz(&f, 0); pb_zz(&f, 3); pb_zz(&f, 8); }
            else { pb_field(&f, &lm, 2, PB_LIST); pb_list(&f, 2, PB_I32); pb_zz(&f, 0); pb_zz(&f, 3); }
            pb_field(&f, &lm, 3, PB_LIST); pb_list(&f, 1, PB_BIN); pb_bin(&f, cols[c], 1);
            pb_i(&f, &lm, 4, PB_I32, 0);
            pb_i(&f, &lm, 5, PB_I64, n);
            pb_i(&f, &lm, 6, PB_I64, sz[i]);
            pb_i(&f, &lm, 7, PB_I64, sz[i]);
            pb_i(&f, &lm, 9, PB_I64, dat[i] >= 0 ? dat[i] : at[i]);
            if (dat[i] >= 0) pb_i(&f, &lm, 11, PB_I64, at[i]);
            if (dat[i] >= 0) pb_enc_stats(&f, &lm, true, true, false);
            pb_byte(&f, 0);   /* meta */
            pb_byte(&f, 0);   /* column chunk */
            total += sz[i];
        }
        pb_i(&f, &l, 2, PB_I64, total);
        pb_i(&f, &l, 3, PB_I64, n);
        pb_byte(&f, 0);
    }
    pb_field(&f, &last, 6, PB_BIN); pb_bin(&f, "rayforce test", 13);
    pb_byte(&f, 0);
    uint32_t flen = (uint32_t)(f.n - (size_t)footer);
    pb_bytes(&f, &flen, 4); pb_bytes(&f, "PAR1", 4);
    bool ok = !f.bad && !page.bad && !levels.bad && !values.bad && !dict.bad && !doff.bad;
    FILE* out = ok ? fopen(path, "wb") : NULL;
    ok = out && fwrite(f.p, 1, f.n, out) == f.n;
    if (out && fclose(out)) ok = false;
    ray_sys_free(f.p); ray_sys_free(page.p); ray_sys_free(levels.p); ray_sys_free(values.p);
    ray_sys_free(dict.p); ray_sys_free(doff.p);
    ray_sys_free(at); ray_sys_free(sz); ray_sys_free(dat);
    return ok;
}
/* The columns s and t of an import as strings: true when every row is
 * fn's (a null and "" both the empty symbol). */
static bool pq_synth_check(const char* dir, int64_t rows, pq_synth_fn fn) {
    char sym[200]; snprintf(sym, sizeof(sym), "%s/.sym", dir);
    ray_t* t = ray_read_splayed(dir, sym);
    if (!t || RAY_IS_ERR(t)) { if (t) ray_error_free(t); return false; }
    bool ok = ray_table_nrows(t) == rows;
    char buf[256];
    for (int c = 1; ok && c < PQ_SYNTH_COLS; c++) {
        ray_t* s = ray_table_get_col_idx(t, c);
        for (int64_t r = 0; ok && r < rows; r++) {
            uint32_t len = 0;
            const char* want = pq_synth_row(fn, c, r, rows, buf, &len);
            if (!want) len = 0;
            ray_t* text = ray_sym_vec_cell(s, r);
            ok = text && ray_str_len(text) == len && (!len || !memcmp(ray_str_ptr(text), want, len));
            if (ok && ((int32_t*)ray_data(ray_table_get_col_idx(t, 0)))[r] != r) ok = false;
        }
    }
    ray_release(t);
    return ok;
}
/* A whole file's bytes compared with another's. */
static bool pq_same_file(const char* a, const char* b) {
    FILE* fa = fopen(a, "rb"); FILE* fb = fopen(b, "rb");
    bool ok = fa && fb;
    while (ok) {
        char x[4096], y[4096];
        size_t na = fread(x, 1, sizeof(x), fa), nb = fread(y, 1, sizeof(y), fb);
        if (na != nb || memcmp(x, y, na)) ok = false;
        if (!na) break;
    }
    if (fa) fclose(fa);
    if (fb) fclose(fb);
    return ok;
}
static test_result_t test_pq_group_native(void) {
    const char* names[] = {"x","y","s"};
    int64_t tids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,tids,3);
    for (int cores = 1; cores <= 8; cores *= 2) {
        TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
        /* 0-3: STR or SYM, one pass or column by column; 5 and 7: SYM
         * through the grouped import */
        for (int run = 0; run < 8; run++) {
            if (run == 4 || run == 6) continue;
            int sym = run & 3;
            pq_set_layout(sym >= 2 ? "1" : NULL);
            pq_set_symmode(run >= 4 ? "grouped" : NULL);
            char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-groups-%d-%d-%d",(int)getpid(),cores,run);
            ray_t* result = ray_parquet_splayed_typed(FIX "row-groups.parquet",dir,sym & 1 ? types : NULL);
            pq_set_layout(NULL); pq_set_symmode(NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(result)); TEST_ASSERT_EQ_I(result->i64,44009); ray_release(result);
            char domain[200]; snprintf(domain,sizeof(domain),"%s/.sym",dir);
            ray_t* table = ray_read_splayed(dir,sym & 1 ? domain : NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(table)); TEST_ASSERT_EQ_I(ray_table_nrows(table),44009);
            ray_t* strings = ray_table_get_col_idx(table,2);
            TEST_ASSERT_EQ_I(strings->type,sym & 1 ? RAY_SYM : RAY_STR);
            for (int64_t i = 0; i < 44009; i++) {
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(table,0)))[i],i);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(table,1)))[i],i%13 ? i*3 : NULL_I64);
                char expected[64]; int n = snprintf(expected,sizeof(expected),"long pooled string row %lld",(long long)i);
                size_t len; const char* text;
                if (sym & 1) { ray_t* atom = ray_sym_vec_cell(strings,i); text = ray_str_ptr(atom); len = ray_str_len(atom); }
                else text = ray_str_vec_get(strings,i,&len);
                TEST_ASSERT_EQ_I(len,n); TEST_ASSERT_TRUE(!memcmp(text,expected,len));
            }
            ray_release(table); pq_remove_native(dir,names,3);
        }
        ray_t* table = ray_parquet_read(FIX "row-groups.parquet",NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(table)); TEST_ASSERT_EQ_I(ray_table_nrows(table),44009);
        for (int64_t i = 0; i < 44009; i++) TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(table,0)))[i],i);
        ray_release(table);
        ray_pool_destroy();
    }
    ray_release(types);
    const char* unames[] = {"day","ts","s"};
    int64_t units[] = {ray_sym_intern("UNIX_DATE",9),ray_sym_intern("UNIX_SECONDS",12),ray_sym_intern("SYM",3)};
    types = ray_vec_from_raw(RAY_SYM,units,3);
    for (int grouped = 0; grouped < 2; grouped++) {
        char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-unix-%d-%d",(int)getpid(),grouped);
        pq_set_symmode(grouped ? "grouped" : NULL);
        ray_t* result = ray_parquet_splayed_typed(FIX "unix.parquet",dir,types);
        pq_set_symmode(NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result)); ray_release(result);
        char domain[200]; snprintf(domain,sizeof(domain),"%s/.sym",dir);
        ray_t* table = ray_read_splayed(dir,domain); TEST_ASSERT_FALSE(RAY_IS_ERR(table));
        TEST_ASSERT_EQ_I(ray_table_get_col_idx(table,0)->type,RAY_DATE);
        TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(table,0)))[1],1);
        TEST_ASSERT_EQ_I(ray_table_get_col_idx(table,1)->type,RAY_TIMESTAMP);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(table,1)))[1],1000000000);
        ray_t* text = ray_sym_vec_cell(ray_table_get_col_idx(table,2),1);
        TEST_ASSERT_TRUE(text && !strcmp(ray_str_ptr(text),"pooled string number two"));
        ray_release(table); pq_remove_native(dir,unames,3);
    }
    ray_release(types);
    PASS();
}
/* The direct import over dictionary pages, stored (flat-0-v1) and Snappy
 * (flat-1-v2): two row groups whose chunks each hold a dictionary page, two
 * dictionary-encoded data pages and a PLAIN one, with nulls and an empty
 * string (both the SYM null), on one worker and four, in one pass and
 * column by column. */
static test_result_t test_pq_native_dictionary(void) {
    const char* files[] = {FIX "flat-0-v1.parquet",FIX "flat-1-v2.parquet"};
    const char* names[] = {"x","name","flag","wide","day","ts","f"};
    const char* strings[] = {"alpha","a long string over twelve bytes","","","alpha","beta","gamma","delta","z"};
    int64_t expect[] = {1,NULL_I32,3,4,5,NULL_I32,7,8,9};
    int64_t ids[] = {ray_sym_intern("I32",3),ray_sym_intern("SYM",3),ray_sym_intern("I16",3),
        ray_sym_intern("I64",3),ray_sym_intern("DATE",4),ray_sym_intern("TIMESTAMP",9),ray_sym_intern("F64",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,ids,7);
    for (int run = 0; run < 16; run++) {   /* 8-15: the grouped import */
        TEST_ASSERT_EQ_I(pq_pool(run & 2 ? 4 : 1), RAY_OK);
        char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-dict-%d-%d",(int)getpid(),run);
        pq_set_layout(run & 4 ? "1" : NULL);
        pq_set_symmode(run & 8 ? "grouped" : NULL);
        ray_t* result = ray_parquet_splayed_typed(files[run & 1],dir,types);
        pq_set_layout(NULL); pq_set_symmode(NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result)); TEST_ASSERT_EQ_I(result->i64,18); ray_release(result);
        char sym[200]; snprintf(sym,sizeof(sym),"%s/.sym",dir);
        ray_t* table = ray_read_splayed(dir,sym);
        TEST_ASSERT_FALSE(RAY_IS_ERR(table)); TEST_ASSERT_EQ_I(ray_table_nrows(table),18);
        ray_t* name = ray_table_get_col_idx(table,1);
        TEST_ASSERT_EQ_I(name->type,RAY_SYM);
        TEST_ASSERT_TRUE(name->attrs & RAY_ATTR_HAS_NULLS);
        for (int64_t i = 0; i < 18; i++) {
            int j = (int)(i%9); size_t len = strlen(strings[j]);
            ray_t* text = ray_sym_vec_cell(name,i);
            TEST_ASSERT_TRUE(text != NULL); TEST_ASSERT_EQ_I(ray_str_len(text),len);
            TEST_ASSERT_TRUE(!len || !memcmp(ray_str_ptr(text),strings[j],len));
            TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(table,0)))[i],expect[j]);
            TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(table,3)))[i],100+j);
        }
        ray_release(table); pq_remove_native(dir,names,7);
        ray_pool_destroy();
    }
    ray_release(types);
    PASS();
}
static test_result_t test_pq_native_edges(void) {
    int64_t ids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,ids,3);
    TEST_ASSERT_EQ_I(pq_pool(4), RAY_OK);
    ray_t* t = ray_parquet_read(FIX "rle-runs.parquet",NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_nrows(t),24597);
    for (int i = 0; i < 24597; i++) {
        bool first = i%8199 < 4099;
        TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(t,0)))[i],first ? 7 : 9);
        TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(t,1)))[i],first ? 11 : 13);
        size_t len; const char* text = ray_str_vec_get(ray_table_get_col_idx(t,2),i,&len);
        TEST_ASSERT_EQ_I(len,first ? 24 : 0);
        if (first) TEST_ASSERT_TRUE(!memcmp(text,"a repeated pooled string",24));
    }
    ray_release(t);
    char dir[160], sym[200];
    for (int grouped = 0; grouped < 2; grouped++) {
        snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-rle-%d-%d",(int)getpid(),grouped);
        pq_set_symmode(grouped ? "grouped" : NULL);
        t = ray_parquet_splayed_typed(FIX "rle-runs.parquet",dir,types);
        pq_set_symmode(NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(t)); ray_release(t);
        snprintf(sym,sizeof(sym),"%s/.sym",dir);
        t = ray_read_splayed(dir,sym); TEST_ASSERT_FALSE(RAY_IS_ERR(t));
        for (int i = 0; i < 24597; i++) {
            ray_t* text = ray_sym_vec_cell(ray_table_get_col_idx(t,2),i);
            TEST_ASSERT_EQ_I(ray_str_len(text),i%8199 < 4099 ? 24 : 0);
        }
        ray_release(t);
        const char* names[] = {"x","y","s"}; pq_remove_native(dir,names,3);
    }
    ray_release(types);
    int64_t empty_ids[] = {ray_sym_intern("I32",3),ray_sym_intern("SYM",3),ray_sym_intern("I16",3),
        ray_sym_intern("I64",3),ray_sym_intern("DATE",4),ray_sym_intern("TIMESTAMP",9),ray_sym_intern("F64",3)};
    types = ray_vec_from_raw(RAY_SYM,empty_ids,7);
    snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-empty-native-%d",(int)getpid());
    pq_set_symmode("grouped");   /* no row group: the grouped import's empty pass */
    t = ray_parquet_splayed_typed(FIX "empty.parquet",dir,types);
    pq_set_symmode(NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(t->i64,0); ray_release(t);
    snprintf(sym,sizeof(sym),"%s/.sym",dir);
    t = ray_read_splayed(dir,sym); TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_nrows(t),0); ray_release(t);
    const char* enames[] = {"x","name","flag","wide","day","ts","f"}; pq_remove_native(dir,enames,7);
    /* A corrupt worker must join the others and leave the final name absent. */
    char partial[200];
    for (int grouped = 0; grouped < 2; grouped++) {
        pq_set_symmode(grouped ? "grouped" : NULL);
        t = ray_parquet_splayed_typed(FIX "projection.parquet",dir,types);
        pq_set_symmode(NULL);
        TEST_ASSERT_TRUE(RAY_IS_ERR(t)); ray_release(t); TEST_ASSERT_TRUE(access(dir,F_OK) != 0);
        snprintf(partial,sizeof(partial),"%s.parquet-partial",dir); pq_remove_native(partial,enames,7);
    }
    /* Typed partition import uses the same native epoch/width contract. */
    snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-typed-parts-%d",(int)getpid());
    t = ray_parquet_parted_typed(FIX "flat-1-v2.parquet",dir,"hits",types);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(t->i64,18); ray_release(t);
    t = ray_read_parted(dir,"hits"); TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    TEST_ASSERT_EQ_I(ray_table_nrows(t),18); ray_release(t);
    snprintf(sym,sizeof(sym),"%s/.sym",dir);
    for (int part = 0; part < 2; part++) {
        char leaf[200]; snprintf(leaf,sizeof(leaf),"%s/%d/hits",dir,part);
        t = ray_read_splayed(leaf,sym); TEST_ASSERT_FALSE(RAY_IS_ERR(t));
        TEST_ASSERT_EQ_I(ray_table_get_col_idx(t,1)->type,RAY_SYM);
        TEST_ASSERT_EQ_I(ray_str_len(ray_sym_vec_cell(ray_table_get_col_idx(t,1),1)),31);
        ray_release(t); pq_remove_native(leaf,enames,7);
        snprintf(leaf,sizeof(leaf),"%s/%d",dir,part); rmdir(leaf);
    }
    pq_remove_native(dir,NULL,0);
    ray_release(types); ray_pool_destroy();
    PASS();
}

static test_result_t test_pq_parted_symbols(void) {
    TEST_ASSERT_EQ_I(pq_pool(8), RAY_OK);
    int64_t ids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,ids,3);
    char root[160],sym[200],leaf[200];
    snprintf(root,sizeof(root),"/tmp/rayforce-pq-parted-symbols-%d",(int)getpid());
    ray_t* result = ray_parquet_parted_typed(FIX "row-groups.parquet",root,"hits",types);
    ray_release(types);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result)); TEST_ASSERT_EQ_I(result->i64,44009); ray_release(result);
    snprintf(sym,sizeof(sym),"%s/.sym",root);
    const int starts[] = {0,5003,15004,22005,27006,33007,39008,44009};
    const char* names[] = {"x","y","s"};
    /* Each group adds unique symbols concurrently. Reopen every partition
     * after the import domain was released, including late dictionary entries. */
    for (int p = 0; p < 7; p++) {
        snprintf(leaf,sizeof(leaf),"%s/%d/hits",root,p);
        ray_t* t = ray_read_splayed(leaf,sym); TEST_ASSERT_FALSE(RAY_IS_ERR(t));
        TEST_ASSERT_EQ_I(ray_table_nrows(t),starts[p+1]-starts[p]);
        ray_t* col = ray_table_get_col_idx(t,2);
        for (int i = starts[p]; i < starts[p+1]; i++) {
            char expected[64]; snprintf(expected,sizeof(expected),"long pooled string row %d",i);
            ray_t* s = ray_sym_vec_cell(col,i-starts[p]); TEST_ASSERT_TRUE(s != NULL);
            TEST_ASSERT_TRUE(!strcmp(ray_str_ptr(s),expected));
        }
        ray_release(t); pq_remove_native(leaf,names,3);
        snprintf(leaf,sizeof(leaf),"%s/%d",root,p); rmdir(leaf);
    }
    pq_remove_native(root,NULL,0); ray_pool_destroy(); PASS();
}
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
static void* pq_small_stack_worker(void* result) {
    pq_setup();
    int64_t ids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    int64_t key = ray_sym_intern("types",5);
    ray_t* values = ray_list_new(1);
    ((ray_t**)ray_data(values))[0] = ray_vec_from_raw(RAY_SYM,ids,3); values->len = 1;
    ray_t* options = ray_dict_new(ray_vec_from_raw(RAY_SYM,&key,1),values);
    const char* files[] = {FIX "unix.parquet",FIX "rle-runs.parquet",FIX "row-groups.parquet"};
    const int64_t counts[] = {2,24597,44009};
    bool ok = true;
    /* No pool: every dictionary/PLAIN page and batch is decoded on this
     * deliberately small thread stack, including the nested dictionary path. */
    for (int f = 0; f < 3 && ok; f++) {
        ray_t* t = ray_parquet_read(files[f],options);
        ok = t && !RAY_IS_ERR(t) && ray_table_nrows(t) == counts[f];
        if (ok) {
            ray_t* col = ray_table_get_col_idx(t,2);
            for (int64_t i = 0; i < counts[f] && ok; i++) {
                char expected[64];
                if (f == 0) snprintf(expected,sizeof(expected),"pooled string number %s",i ? "two" : "one");
                else if (f == 1) snprintf(expected,sizeof(expected),"%s",i%8199 < 4099 ? "a repeated pooled string" : "");
                else snprintf(expected,sizeof(expected),"long pooled string row %lld",(long long)i);
                ray_t* text = ray_sym_vec_cell(col,i);
                ok = text && !strcmp(ray_str_ptr(text),expected);
            }
        }
        if (t) ray_release(t);
    }
    ray_release(options); pq_teardown(); *(bool*)result = ok;
    return NULL;
}
#endif
/* Direct path: 7 uneven row groups merge into the whole-column zone; the
 * unclustered column with nulls gets a hash. */
static test_result_t test_pq_splayed_inline_indexes(void) {
    const char* names[] = {"x","y"};
    for (int run = 0; run < 4; run++) {
        int cores = run & 1 ? 4 : 1;
        TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
        char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-zones-%d-%d",(int)getpid(),run);
        pq_set_layout(run >= 2 ? "1" : NULL);
        ray_t* result = ray_parquet_splayed_typed(FIX "zones.parquet",dir,NULL);
        pq_set_layout(NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(result)); TEST_ASSERT_EQ_I(result->i64,200000); ray_release(result);
        ray_t* table = ray_read_splayed(dir,NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(table));
        ray_t* x = ray_table_get_col_idx(table,0);
        TEST_ASSERT_EQ_I(ray_index_kind(x),RAY_IDX_CHUNK_ZONE);
        ray_t* plain = ray_vec_from_raw(RAY_I32,ray_data(x),x->len);
        ray_t* want = ray_index_chunk_zone_compute(plain,16);
        TEST_ASSERT_TRUE(want && !RAY_IS_ERR(want));
        const ray_index_t* g = ray_index_payload(x->index);
        const ray_index_t* w = ray_index_payload(want);
        TEST_ASSERT_EQ_I(g->u.chunk_zone.n_chunks,4);
        TEST_ASSERT_EQ_I(memcmp(ray_data(g->u.chunk_zone.mins),ray_data(w->u.chunk_zone.mins),4*8),0);
        TEST_ASSERT_EQ_I(memcmp(ray_data(g->u.chunk_zone.maxs),ray_data(w->u.chunk_zone.maxs),4*8),0);
        TEST_ASSERT_EQ_I(memcmp(ray_data(g->u.chunk_zone.aggs),ray_data(w->u.chunk_zone.aggs),12*8),0);
        TEST_ASSERT_TRUE((g->u.chunk_zone.null_bits != NULL) == (w->u.chunk_zone.null_bits != NULL));
        if (w->u.chunk_zone.null_bits)
            TEST_ASSERT_EQ_I(memcmp(ray_data(g->u.chunk_zone.null_bits),ray_data(w->u.chunk_zone.null_bits),(size_t)((g->u.chunk_zone.n_chunks+7)/8)),0);
        ray_release(want); ray_release(plain);
        TEST_ASSERT_EQ_I(ray_index_kind(ray_table_get_col_idx(table,1)),RAY_IDX_HASH);
        ray_release(table); pq_remove_native(dir,names,2);
        ray_pool_destroy();
    }
    PASS();
}

static test_result_t test_pq_small_stack(void) {
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
    ray_pool_destroy();
    pthread_attr_t attr; pthread_t worker; bool ok = false;
    TEST_ASSERT_EQ_I(pthread_attr_init(&attr),0);
    int rc = pthread_attr_setstacksize(&attr,256*1024);
    if (!rc) rc = pthread_create(&worker,&attr,pq_small_stack_worker,&ok);
    pthread_attr_destroy(&attr);
    TEST_ASSERT_EQ_I(rc,0);
    TEST_ASSERT_EQ_I(pthread_join(worker,NULL),0);
    TEST_ASSERT_TRUE(ok);
    PASS();
#else
    SKIP("requires POSIX thread stack attributes");
#endif
}
#if defined(__linux__)
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
/* A row group's chunk ends with a flush of the bytes its stream still
 * buffers (glibc writes whole blocks of a large write at once and keeps the
 * tail).  The file size limit here falls inside that tail of the first
 * chunk of x (rows 0-5002, the first task), so it is the flush that fails:
 * the import must report that chunk's failure, not a later write's, and
 * publish nothing. */
static test_result_t test_pq_chunk_flush_error(void) {
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int64_t tids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,tids,3);
    char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-flush-%d",(int)getpid());
    struct rlimit old, lim;
    TEST_ASSERT_EQ_I(getrlimit(RLIMIT_FSIZE,&old),0);
    lim = old; lim.rlim_cur = 32 + 5003*4 - 1;
    void (*prev)(int) = signal(SIGXFSZ,SIG_IGN);
    pq_set_layout(NULL);
    TEST_ASSERT_EQ_I(setrlimit(RLIMIT_FSIZE,&lim),0);
    ray_t* result = ray_parquet_splayed_typed(FIX "row-groups.parquet",dir,types);
    setrlimit(RLIMIT_FSIZE,&old);
    signal(SIGXFSZ,prev);
    ray_release(types);
    bool failed = RAY_IS_ERR(result);
    const char* code = failed ? ray_err_code(result) : "";
    bool on_chunk = failed && code && strcmp(code,"parquet") == 0;   /* the flush, not a later write */
    if (failed) ray_error_free(result); else ray_release(result);
    struct stat st;
    bool published = stat(dir,&st) == 0;
    char staging[200]; snprintf(staging,sizeof(staging),"%s.parquet-partial",dir);
    const char* names[] = {"x","y","s"};
    pq_remove_native(dir,names,3); pq_remove_native(staging,names,3);
    ray_pool_destroy();
    TEST_ASSERT_TRUE(failed);
    TEST_ASSERT_FALSE(published);
    TEST_ASSERT_TRUE(on_chunk);
    PASS();
}
#endif

/* Rows for the grouped import: repeats inside and across row groups,
 * nulls, "" and strings past the inline bytes. */
static const char* pq_synth_mixed(int64_t r, char* buf, uint32_t* len) {
    if (r % 17 == 0) return NULL;
    if (r % 23 == 0) { *len = 0; return buf; }
    int n = snprintf(buf, 256, "v%lld-%s", (long long)((r * 7919) % 3001),
                     r % 3 ? "s" : "a longer tail past the inline bytes");
    *len = (uint32_t)n;
    return buf;
}
/* 48 strings of one length told apart by one byte: the first, the middle
 * or the last. */
static const char* pq_synth_alike(int64_t r, char* buf, uint32_t* len) {
    if (r % 29 == 0) return NULL;
    memcpy(buf, "abcdefghijklmnopqrstuvwxyz012345", 32);
    int v = (int)((r * 13) % 48), at = v < 16 ? 0 : v < 32 ? 16 : 31;
    buf[at] = (char)('A' + v % 16);
    *len = 32;
    return buf;
}
/* A unique 100-byte string a row. */
static const char* pq_synth_long(int64_t r, char* buf, uint32_t* len) {
    int n = snprintf(buf, 256, "%08lld", (long long)r);
    memset(buf + n, 'x', (size_t)(100 - n));
    *len = 100;
    return buf;
}
/* An import with RAY_CSV_TRACE set, its trace (stderr) kept in buf, which
 * ends with a NUL; buf is empty when stderr cannot be redirected. */
static ray_t* pq_traced_import(const char* src, const char* dir, ray_t* types, char* buf, size_t cap) {
    char path[160]; snprintf(path, sizeof(path), "/tmp/rayforce-pq-trace-%d.txt", (int)getpid());
    buf[0] = 0;
    fflush(stderr);
    int saved = dup(2), fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    bool redirected = saved >= 0 && fd >= 0 && dup2(fd, 2) >= 0;
    if (fd >= 0) close(fd);
    pq_set_env("RAY_CSV_TRACE", "1");
    ray_t* res = ray_parquet_splayed_typed(src, dir, types);
    pq_set_env("RAY_CSV_TRACE", NULL);
    fflush(stderr);
    if (redirected) dup2(saved, 2);
    if (saved >= 0) close(saved);
    FILE* f = redirected ? fopen(path, "rb") : NULL;
    size_t n = f ? fread(buf, 1, cap - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = 0;
    unlink(path);
    return res;
}
/* The sum of a trace field (" key=value") over the trace's lines, and how
 * many lines have it. */
static double pq_trace_sum(const char* buf, const char* key, int* lines) {
    char pat[64]; snprintf(pat, sizeof(pat), " %s=", key);
    size_t pl = strlen(pat);
    double sum = 0; int n = 0;
    for (const char* p = buf; (p = strstr(p, pat)); p += pl) { sum += strtod(p + pl, NULL); n++; }
    if (lines) *lines = n;
    return sum;
}
/* Whether the trace's first line starting with `line` has `token` in it. */
static bool pq_trace_line_has(const char* buf, const char* line, const char* token) {
    const char* p = strstr(buf, line);
    if (!p) return false;
    const char* e = strchr(p, '\n');
    const char* q = strstr(p, token);
    return q && (!e || q < e);
}
/* A field of the trace's first line starting with `line` (" key=value"),
 * -1 when there is none. */
static double pq_trace_line_num(const char* buf, const char* line, const char* key) {
    const char* p = strstr(buf, line);
    if (!p) return -1;
    const char* e = strchr(p, '\n');
    char pat[64]; snprintf(pat, sizeof(pat), " %s=", key);
    const char* q = strstr(p, pat);
    return q && (!e || q < e) ? strtod(q + strlen(pat), NULL) : -1;
}
/* The entries of an import's symbol file (its header count). */
static int64_t pq_sym_count(const char* dir) {
    char path[200]; snprintf(path, sizeof(path), "%s/.sym", dir);
    FILE* f = fopen(path, "rb");
    uint8_t head[12];
    int64_t count = -1;
    if (f && fread(head, 1, 12, f) == 12) memcpy(&count, head + 4, 8);
    if (f) fclose(f);
    return count;
}

/* The grouped import of a 9-row-group file: every row's string, the same
 * vocabulary as the direct import's, and positions (the symbol file and the
 * column file byte for byte) that do not depend on the worker count, in
 * every order of new positions, over several hash groups and windows.  Its
 * trace: no collision on this data (a comparison that never matched would
 * still give every string a position, through the collision path), and
 * every record's writeback started by the window that wrote it (exactly
 * the records' bytes in the orders that write each window's records side
 * by side). */
static test_result_t test_pq_sym_grouped(void) {
    pq_sym_env_clear();
    char src[160]; snprintf(src, sizeof(src), "/tmp/rayforce-pq-synth-%d.parquet", (int)getpid());
    const int64_t rows = 20000;
    TEST_ASSERT_TRUE(pq_synth(src, rows, 9, pq_synth_mixed));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    const char* orders[] = {"rows","shards","freq","rowsflat"};
    char dir[160], ref[160], a[200], b[200];
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-sg-direct-%d", (int)getpid());
    pq_set_symmode("direct");
    ray_t* res = ray_parquet_splayed_typed(src, dir, types);
    pq_set_symmode(NULL);
    TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
    TEST_ASSERT_TRUE(pq_synth_check(dir, rows, pq_synth_mixed));
    int64_t words = pq_sym_count(dir);
    TEST_ASSERT_TRUE(words > 1000);
    pq_remove_native(dir, names, 3);
    pq_set_env("RAY_PQ_SYM_GROUPS", "4");
    pq_set_env("RAY_PQ_SYM_WINDOW", "4096");   /* debug builds: several windows */
    for (int o = 0; o < 4; o++) {
        pq_set_env("RAY_PQ_SYM_ORDER", orders[o]);
        snprintf(ref, sizeof(ref), "/tmp/rayforce-pq-sg-%d-%d-1", (int)getpid(), o);
        for (int cores = 1; cores <= 8; cores *= 2) {
            TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
            snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-sg-%d-%d-%d", (int)getpid(), o, cores);
            pq_set_symmode("grouped"); pq_set_layout(cores == 2 ? "1" : NULL);
            res = pq_traced_import(src, dir, types, trace, cap);
            pq_set_symmode(NULL); pq_set_layout(NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(res)); TEST_ASSERT_EQ_I(res->i64, rows); ray_release(res);
            TEST_ASSERT_TRUE(pq_synth_check(dir, rows, pq_synth_mixed));
            TEST_ASSERT_EQ_I(pq_sym_count(dir), words);   /* no string twice */
            int passes = 0;
            double windows = pq_trace_sum(trace, "windows", &passes);
            double recb = pq_trace_sum(trace, "rec_bytes", NULL), wbb = pq_trace_sum(trace, "wb_bytes", NULL);
            TEST_ASSERT_EQ_I(passes, 2);
            TEST_ASSERT_TRUE(windows > 2 * passes);
            TEST_ASSERT_TRUE(pq_trace_sum(trace, "collisions", NULL) == 0);
            TEST_ASSERT_TRUE(recb > 0);
            if (o == 0 || o == 3) TEST_ASSERT_TRUE(wbb == recb);
            else TEST_ASSERT_TRUE(wbb >= recb);
            if (cores > 1) {
                snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
                TEST_ASSERT_TRUE(pq_same_file(a, b));
                for (int c = 1; c < 3; c++) {
                    snprintf(a, sizeof(a), "%s/%s", ref, names[c]); snprintf(b, sizeof(b), "%s/%s", dir, names[c]);
                    TEST_ASSERT_TRUE(pq_same_file(a, b));
                }
                pq_remove_native(dir, names, 3);
            }
            ray_pool_destroy();
        }
        pq_remove_native(ref, names, 3);
    }
    pq_set_env("RAY_PQ_SYM_ORDER", NULL); pq_set_env("RAY_PQ_SYM_GROUPS", NULL);
    pq_set_env("RAY_PQ_SYM_WINDOW", NULL);
    ray_sys_free(trace);
    ray_release(types); unlink(src);
    PASS();
}

/* Hash collisions on purpose (a debug build keeps none of the hash's bits):
 * 48 strings of one length that differ only in their first, middle or last
 * byte all share a hash, and the dedupe arena is small enough to start
 * over inside a chunk.  Every candidate is then compared, most mismatch,
 * and each string still gets one position of its own, in every order, on
 * one worker and four; with some of the bits kept (the low ones, or the
 * top ones over several hash groups) the vocabulary is the
 * direct import's. */
static test_result_t test_pq_sym_grouped_collisions(void) {
#if !defined(DEBUG)
    SKIP("hash truncation is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], mix[160], dir[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-alike-%d.parquet", (int)getpid());
    snprintf(mix, sizeof(mix), "/tmp/rayforce-pq-mixed-%d.parquet", (int)getpid());
    TEST_ASSERT_TRUE(pq_synth(src, 3000, 5, pq_synth_alike));
    TEST_ASSERT_TRUE(pq_synth(mix, 6000, 4, pq_synth_mixed));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    const char* orders[] = {"rows","shards","freq","rowsflat"};
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-coll-%d", (int)getpid());
    ray_t* res = ray_parquet_splayed_typed(mix, dir, types);   /* direct */
    TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
    int64_t words = pq_sym_count(dir);
    pq_remove_native(dir, names, 3);
    pq_set_env("RAY_PQ_SYM_ARENA", "200");
    /* one window (candidates of other tasks deferred, mismatches redone)
     * or a window a task (candidates compared with records read before) */
    for (int run = 0; run < 16; run++) {
            int o = run % 4, cores = run & 4 ? 4 : 1;
            pq_set_env("RAY_PQ_SYM_WINDOW", run >= 8 ? "64" : NULL);
            TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
            pq_set_env("RAY_PQ_SYM_ORDER", orders[o]);
            pq_set_env("RAY_PQ_SYM_HASH_BITS", "0");
            pq_set_symmode("grouped");
            char trace[8192];
            res = pq_traced_import(src, dir, types, trace, sizeof(trace));
            TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
            TEST_ASSERT_TRUE(pq_synth_check(dir, 3000, pq_synth_alike));
            TEST_ASSERT_EQ_I(pq_sym_count(dir), 49);   /* "" and the 48 */
            /* the trace counts the collisions, and the writeback covers the
             * records they added */
            TEST_ASSERT_TRUE(pq_trace_sum(trace, "collisions", NULL) > 0);
            double recb = pq_trace_sum(trace, "rec_bytes", NULL), wbb = pq_trace_sum(trace, "wb_bytes", NULL);
            TEST_ASSERT_TRUE(o == 0 || o == 3 ? wbb == recb : wbb >= recb);
            /* the second column's pass takes nothing into its index: the
             * first's records are there, those its collisions added too */
            TEST_ASSERT_TRUE(pq_trace_sum(trace, "adopted", NULL) == 0);
            pq_remove_native(dir, names, 3);
            /* six low bits of the hash kept, or (a run in two) its two top
             * bits with four hash groups: collisions in every group, not only
             * the first */
            bool top = (run & 2) != 0;
            pq_set_env("RAY_PQ_SYM_HASH_BITS", top ? NULL : "6");
            pq_set_env("RAY_PQ_SYM_HASH_TOP", top ? "2" : NULL);
            pq_set_env("RAY_PQ_SYM_GROUPS", top ? "4" : NULL);
            res = pq_traced_import(mix, dir, types, trace, sizeof(trace));
            pq_set_env("RAY_PQ_SYM_HASH_TOP", NULL); pq_set_env("RAY_PQ_SYM_GROUPS", NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
            TEST_ASSERT_TRUE(pq_synth_check(dir, 6000, pq_synth_mixed));
            TEST_ASSERT_EQ_I(pq_sym_count(dir), words);
            if (top) TEST_ASSERT_TRUE(pq_trace_sum(trace, "collisions", NULL) > 0);
            TEST_ASSERT_TRUE(pq_trace_sum(trace, "adopted", NULL) == 0);
            pq_remove_native(dir, names, 3);
            ray_pool_destroy();
        }
    pq_set_symmode(NULL);
    pq_set_env("RAY_PQ_SYM_ORDER", NULL); pq_set_env("RAY_PQ_SYM_HASH_BITS", NULL);
    pq_set_env("RAY_PQ_SYM_ARENA", NULL); pq_set_env("RAY_PQ_SYM_WINDOW", NULL);
    ray_release(types); unlink(src); unlink(mix);
    PASS();
#endif
}

/* Strings as a column of addresses has them: half the rows a string of
 * their own, the rest drawn from a few heavy hitters and a long tail
 * (k uniform below m, m uniform in 1..4000), 20 to 119 bytes. */
static const char* pq_synth_zipf(int64_t r, char* buf, uint32_t* len) {
    uint64_t x = (uint64_t)r * UINT64_C(0x9E3779B97F4A7C15);
    x ^= x >> 29; x *= UINT64_C(0xBF58476D1CE4E5B9); x ^= x >> 32;
    uint64_t key = x & 1 ? (uint64_t)r : (x >> 1) % (1 + (x >> 40) % 4000);
    int n = snprintf(buf, 256, "%c%llu/", x & 1 ? 'u' : 'z', (unsigned long long)key);
    int want = 20 + (int)((key * 7919) % 100);
    if (n < want) { memset(buf + n, 'p', (size_t)(want - n)); n = want; }
    *len = (uint32_t)n;
    return buf;
}
/* The positions the grouped import gives a pass's new strings: under
 * "rows" first the strings a later row group meets again, then the others,
 * each part in first-occurrence order; under "rowsflat" all of them in
 * first-occurrence order.  On one worker and four, over several windows
 * (debug builds), the vocabulary the direct import's. */
static test_result_t test_pq_sym_grouped_layout(void) {
    pq_sym_env_clear();
    char src[160], dir[160], sym[200];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-zipf-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-layout-%d", (int)getpid());
    snprintf(sym, sizeof(sym), "%s/.sym", dir);
    const int64_t rows = 24000, groups = 12;
    TEST_ASSERT_TRUE(pq_synth(src, rows, groups, pq_synth_zipf));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    ray_t* res = ray_parquet_splayed_typed(src, dir, types);   /* direct */
    TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
    int64_t words = pq_sym_count(dir);
    pq_remove_native(dir, names, 3);
    pq_set_env("RAY_PQ_SYM_WINDOW", "16384");
    for (int run = 0; run < 4; run++) {
        bool split = run < 2;
        TEST_ASSERT_EQ_I(pq_pool(run & 1 ? 4 : 1), RAY_OK);
        pq_set_env("RAY_PQ_SYM_ORDER", split ? "rows" : "rowsflat");
        pq_set_env("RAY_PQ_SYM_GROUPS", run & 1 ? "64" : NULL);   /* a group table over and over */
        pq_set_symmode("grouped");
        /* a dedupe arena of 100000 bytes a generation: past the 64 KB it
         * starts with, doubled no further than that */
        pq_set_env("RAY_PQ_SYM_ARENA", "100000");
        char trace[8192];
        res = pq_traced_import(src, dir, types, trace, sizeof(trace));
        pq_set_env("RAY_PQ_SYM_ARENA", NULL);
        pq_set_symmode(NULL);
        TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
        TEST_ASSERT_TRUE(pq_synth_check(dir, rows, pq_synth_zipf));
        int gpasses = 0, workers = run & 1 ? 4 : 1;
        double arena = pq_trace_sum(trace, "arena_kb", &gpasses);
        if (arena > gpasses * workers * 98.0 || arena <= gpasses * 64.0) fprintf(stderr, "  layout %d: arenas %.0f KB in %d passes\n", run, arena, gpasses);
        TEST_ASSERT_TRUE(gpasses == 2 && arena > gpasses * 64.0 && arena <= gpasses * workers * 98.0);
        TEST_ASSERT_EQ_I(pq_sym_count(dir), words);
        /* the column of the first pass (the smaller chunks), the one whose
         * strings hold positions 1 to their count: their first row and
         * whether a later row group has them too, by position */
        ray_t* t = ray_read_splayed(dir, sym);
        TEST_ASSERT_FALSE(!t || RAY_IS_ERR(t));
        int64_t* first = (int64_t*)ray_sys_alloc((size_t)words * sizeof(int64_t));
        uint8_t* again = (uint8_t*)ray_sys_alloc((size_t)words);
        TEST_ASSERT_TRUE(first && again);
        int64_t met = 0, others = 0, passes = 0;
        bool order = true, parted = true;
        for (int c = 1; c < 3; c++) {
            ray_t* s = ray_table_get_col_idx(t, c);
            int64_t distinct = 0, run1 = 0;
            for (int64_t p = 0; p < words; p++) { first[p] = -1; again[p] = 0; }
            for (int64_t r = 0; r < rows; r++) {
                int64_t p = ray_read_sym(ray_data(s), r, RAY_SYM, s->attrs);
                if (p <= 0 || p >= words) continue;
                if (first[p] < 0) { first[p] = r; distinct++; }
                else if (first[p] * groups / rows != r * groups / rows) again[p] = 1;
            }
            while (run1 + 1 < words && first[run1 + 1] >= 0) run1++;
            if (run1 != distinct) continue;
            passes++;
            int64_t last[2] = {-1, -1};
            for (int64_t p = 1; p <= run1; p++) {
                int a = split ? again[p] : 0;
                if (first[p] < last[a]) order = false;
                last[a] = first[p];
                if (again[p]) met++; else others++;
                if (split && again[p] && others) parted = false;
            }
        }
        ray_sys_free(first); ray_sys_free(again);
        ray_release(t);
        TEST_ASSERT_EQ_I(passes, 1);
        TEST_ASSERT_TRUE(met > 100 && others > 100);
        TEST_ASSERT_TRUE(order);
        TEST_ASSERT_TRUE(parted);
        pq_remove_native(dir, names, 3);
        ray_pool_destroy();
    }
    pq_set_env("RAY_PQ_SYM_ORDER", NULL); pq_set_env("RAY_PQ_SYM_WINDOW", NULL);
    pq_set_env("RAY_PQ_SYM_GROUPS", NULL);
    ray_release(types); unlink(src);
    PASS();
}

/* A low-cardinality column (40 strings, nulls, dictionary-encoded) and a
 * high-cardinality one: a string of its own a row but every fifth, which
 * is one of the other column's, or (sparse) one in twenty rows, 34 bytes
 * of its own, the rest null: fewer bytes than the other column's, so it
 * comes first by size. */
static const char* pq_synth_lowcard(int64_t r, char* buf, uint32_t* len) {
    if (r % 31 == 0) return NULL;
    *len = (uint32_t)snprintf(buf, 256, "c%02lld", (long long)((r * 7) % 40));
    return buf;
}
static const char* pq_synth_highcard(int64_t r, char* buf, uint32_t* len) {
    if (r % 5 == 0) return pq_synth_lowcard(r + 1, buf, len);
    *len = (uint32_t)snprintf(buf, 256, "u%06lld-high", (long long)r);
    return buf;
}
static const char* pq_synth_sparse(int64_t r, char* buf, uint32_t* len) {
    if (r % 20) return NULL;
    *len = (uint32_t)snprintf(buf, 256, "sparse-%06lld-and-twenty-bytes", (long long)r);
    return buf;
}
/* Grouped or direct, column by column: with symbol text past a quarter of
 * the memory the decision assumes (RAY_PQ_SYM_RAM, debug builds) the
 * low-cardinality column still goes direct, decoded once, ahead of the
 * grouped one (the sparse one included, smaller as it is), which takes its
 * strings into its index: every row's string and the direct import's
 * vocabulary (none twice), on one worker and four the same files (the
 * direct pass's dictionaries seeded in row-group order); the trace
 * tells which column went which way.  Forced grouped, both columns are. */
static test_result_t test_pq_sym_mixed_modes(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-mixmode-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-mixmode-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-pq-mixmode-%d-ref", (int)getpid());
    int64_t rows = 20000;
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    int bad = 0;
    for (int v = 0; v < 2; v++) {
        pq_synth_t = v ? pq_synth_sparse : pq_synth_highcard; pq_synth_dict = 1u << 1;
        /* the sparse variant twice the rows in half the row groups: its text
         * past a quarter of memory with s's bound in a sixty-fourth */
        rows = v ? 40000 : 20000;
        bool made = pq_synth(src, rows, v ? 2 : 4, pq_synth_lowcard);
        TEST_ASSERT_TRUE(made);
        TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
        pq_set_symmode("direct");
        ray_t* res = pq_traced_import(src, dir, types, trace, cap);
        pq_set_symmode(NULL);
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        TEST_ASSERT_TRUE(pq_synth_check(dir, rows, pq_synth_lowcard));
        int64_t words = pq_sym_count(dir);
        pq_remove_native(dir, names, 3);
        /* the sparse column first by size */
        if (v && !(strstr(trace, "parquet symcol: col=t ") < strstr(trace, "parquet symcol: col=s "))) bad++;
        /* the memory the decision assumes: s's bound just within its
         * sixty-fourth, the symbol text past its quarter, t's bound past
         * its sixty-fourth (the footer's bounds, from the trace) */
        double bs = pq_trace_line_num(trace, "parquet symcol: col=s ", "vocab_bytes");
        double bt = pq_trace_line_num(trace, "parquet symcol: col=t ", "vocab_bytes");
        double text = pq_trace_line_num(trace, "parquet symcol: col=s ", "text_bytes") +
                      pq_trace_line_num(trace, "parquet symcol: col=t ", "text_bytes");
        int64_t sram = (int64_t)bs * 64;
        char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
        if (bs <= 0 || sram / 4 >= text || bt <= sram / 64) {
            fprintf(stderr, "  %d: no memory size mixes them: s %.0f, t %.0f, text %.0f\n", v, bs, bt, text);
            bad++;
        }
        for (int run = 0; run < 4; run++) {
            /* one worker's files kept for four workers' and for one worker's
             * row groups last to first (a debug knob) */
            const char* out = run ? dir : ref;
            TEST_ASSERT_EQ_I(pq_pool(run == 1 ? 4 : 1), RAY_OK);
            pq_set_env("RAY_PQ_SYM_RAM", ramv);
            pq_set_symmode(run == 3 ? "grouped" : NULL);
            if (run == 2) pq_set_env("RAY_PQ_DIRECT_REVERSE", "1");
            res = pq_traced_import(src, out, types, trace, cap);
            pq_sym_env_clear();
            if (!res || RAY_IS_ERR(res)) {
                fprintf(stderr, "  %d/%d: import failed: %s\n", v, run, res ? ray_err_code(res) : "null");
                if (res) ray_error_free(res);
                char partial[200]; snprintf(partial, sizeof(partial), "%s.parquet-partial", out);
                pq_remove_native(partial, names, 3);
                bad++; continue;
            }
            ray_release(res);
            if (!pq_synth_check(out, rows, pq_synth_lowcard)) { fprintf(stderr, "  %d/%d: rows differ\n", v, run); bad++; }
            if (pq_sym_count(out) != words) { fprintf(stderr, "  %d/%d: %lld symbols, direct %lld\n", v, run, (long long)pq_sym_count(out), (long long)words); bad++; }
            bool s_direct = pq_trace_line_has(trace, "parquet symcol: col=s ", " mode=direct");
            bool t_grouped = pq_trace_line_has(trace, "parquet symcol: col=t ", " mode=grouped");
            int passes = 0;
            double adopted = pq_trace_sum(trace, "adopted", &passes);
            fprintf(stderr, "  %d/%d: s %s, t %s, grouped passes %d, adopted %.0f\n", v, run, s_direct ? "direct" : "grouped",
                    t_grouped ? "grouped" : "direct", passes, adopted);
            /* forced: the second pass adopts nothing, the first's records in its
             * index already */
            if (run < 3 ? !s_direct || !t_grouped || passes != 1 || adopted != 40 : s_direct || !t_grouped || passes != 2 || adopted != 0) bad++;
            /* the direct passes' shard tables freed at the first reservation:
             * none through the grouped passes */
            double tabs = pq_trace_sum(trace, "imp_index_kb", NULL);
            if (tabs != 0) { fprintf(stderr, "  %d/%d: the import dictionary's shard tables kept, %.0f KB\n", v, run, tabs); bad++; }
            if (run == 1 || run == 2) {
                /* the direct pass's positions too: s's dictionaries seeded
                 * in row-group order */
                char a[220], b[220];
                bool same = true;
                snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
                if (!pq_same_file(a, b)) same = false;
                for (int c = 1; c < 3; c++) {
                    snprintf(a, sizeof(a), "%s/%s", ref, names[c]); snprintf(b, sizeof(b), "%s/%s", dir, names[c]);
                    if (!pq_same_file(a, b)) same = false;
                }
                if (!same) { fprintf(stderr, "  %d/%d: files differ from one worker's\n", v, run); bad++; }
                if (run == 2) pq_remove_native(ref, names, 3);
            }
            if (run) pq_remove_native(dir, names, 3);
        }
        unlink(src);
    }
    pq_synth_t = NULL; pq_synth_dict = 0;
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}

/* The grouped import failing: out of memory in each of its steps (a debug
 * build injects it), its record writes failing, cancelled in the middle,
 * and the symbol file unable to grow (the file size limit; the direct
 * import too).  Each reports its error and publishes nothing. */
static test_result_t test_pq_sym_grouped_failures(void) {
    pq_sym_env_clear();
    char src[160], big[160], dir[160], partial[200];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-fail-%d.parquet", (int)getpid());
    snprintf(big, sizeof(big), "/tmp/rayforce-pq-fail-big-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-fail-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    TEST_ASSERT_TRUE(pq_synth(src, 20000, 9, pq_synth_mixed));
    TEST_ASSERT_TRUE(pq_synth(big, 20000, 4, pq_synth_long));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    TEST_ASSERT_EQ_I(pq_pool(4), RAY_OK);
#if defined(DEBUG)
    const char* steps[] = {"r1","p2","load","reload","r2","write","settle","cancel"};
    for (int i = 0; i < 8; i++) {
        pq_set_symmode("grouped");
        pq_set_env("RAY_PQ_SYM_INJECT", steps[i]);
        pq_set_env("RAY_PQ_SYM_WINDOW", "4096");   /* a window a row group: "reload" fails one */
        ray_t* res = ray_parquet_splayed_typed(src, dir, types);
        pq_set_env("RAY_PQ_SYM_INJECT", NULL); pq_set_env("RAY_PQ_SYM_WINDOW", NULL); pq_set_symmode(NULL);
        bool failed = res && RAY_IS_ERR(res);
        bool cancel = failed && !strcmp(ray_err_code(res), "cancel");
        if (res) { if (failed) ray_error_free(res); else ray_release(res); }
        ray_clear_interrupt();
        TEST_ASSERT_TRUE(failed);
        TEST_ASSERT_TRUE(cancel == (i == 7));
        TEST_ASSERT_TRUE(access(dir, F_OK) != 0);
        pq_remove_native(partial, names, 3);
    }
#endif
#if defined(__linux__)
    /* 2 MB of records; the column files are 80 KB */
    for (int grouped = 0; grouped < 2; grouped++) {
        struct rlimit old, lim;
        TEST_ASSERT_EQ_I(getrlimit(RLIMIT_FSIZE, &old), 0);
        lim = old; lim.rlim_cur = 1536 << 10;
        void (*prev)(int) = signal(SIGXFSZ, SIG_IGN);
        pq_set_symmode(grouped ? "grouped" : "direct");
        TEST_ASSERT_EQ_I(setrlimit(RLIMIT_FSIZE, &lim), 0);
        ray_t* res = ray_parquet_splayed_typed(big, dir, types);
        setrlimit(RLIMIT_FSIZE, &old);
        signal(SIGXFSZ, prev);
        pq_set_symmode(NULL);
        bool failed = res && RAY_IS_ERR(res);
        if (res) { if (failed) ray_error_free(res); else ray_release(res); }
        TEST_ASSERT_TRUE(failed);
        TEST_ASSERT_TRUE(access(dir, F_OK) != 0);
        pq_remove_native(partial, names, 3);
    }
#endif
    ray_pool_destroy();
    ray_release(types); unlink(src); unlink(big);
    PASS();
}

/* One row group of six rows of a required BYTE_ARRAY column s, every row
 * the first entry of a stored (uncompressed) dictionary page of two entries
 * whose header declares `declared` decoded bytes and `count` entries. */
static bool pq_dict_file(const char* path, int64_t declared, int64_t count) {
    pq_buf f = {0}, dict = {0}, data = {0};
    const char* entries[] = {"first", "second-entry-longer"};
    for (int k = 0; k < 2; k++) {
        uint32_t l = (uint32_t)strlen(entries[k]);
        pb_bytes(&dict, &l, 4); pb_bytes(&dict, entries[k], l);
    }
    const int64_t rows = 6;
    pb_byte(&data, 1); pb_var(&data, (uint64_t)rows << 1); pb_byte(&data, 0);   /* width 1, a run of id 0 */
    pb_bytes(&f, "PAR1", 4);
    int64_t start = (int64_t)f.n;
    int last = 0;
    pb_i(&f, &last, 1, PB_I32, 2); pb_i(&f, &last, 2, PB_I32, declared); pb_i(&f, &last, 3, PB_I32, (int64_t)dict.n);
    pb_field(&f, &last, 7, PB_STRUCT);
    { int l = 0; pb_i(&f, &l, 1, PB_I32, count); pb_i(&f, &l, 2, PB_I32, 0); pb_byte(&f, 0); }
    pb_byte(&f, 0);
    pb_bytes(&f, dict.p, dict.n);
    int64_t data_off = (int64_t)f.n;
    last = 0;
    pb_i(&f, &last, 1, PB_I32, 0); pb_i(&f, &last, 2, PB_I32, (int64_t)data.n); pb_i(&f, &last, 3, PB_I32, (int64_t)data.n);
    pb_field(&f, &last, 5, PB_STRUCT);
    { int l = 0; pb_i(&f, &l, 1, PB_I32, rows); pb_i(&f, &l, 2, PB_I32, 8); pb_i(&f, &l, 3, PB_I32, 3); pb_i(&f, &l, 4, PB_I32, 3); pb_byte(&f, 0); }
    pb_byte(&f, 0);
    pb_bytes(&f, data.p, data.n);
    int64_t chunk = (int64_t)f.n - start, footer = (int64_t)f.n;
    last = 0;
    pb_i(&f, &last, 1, PB_I32, 1);
    pb_field(&f, &last, 2, PB_LIST); pb_list(&f, 2, PB_STRUCT);
    { int l = 0; pb_i(&f, &l, 3, PB_I32, 0); pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, "schema", 6); pb_i(&f, &l, 5, PB_I32, 1); pb_byte(&f, 0); }
    { int l = 0; pb_i(&f, &l, 1, PB_I32, 6); pb_i(&f, &l, 3, PB_I32, 0); pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, "s", 1); pb_i(&f, &l, 6, PB_I32, 0); pb_byte(&f, 0); }
    pb_i(&f, &last, 3, PB_I64, rows);
    pb_field(&f, &last, 4, PB_LIST); pb_list(&f, 1, PB_STRUCT);
    {
        int l = 0;
        pb_field(&f, &l, 1, PB_LIST); pb_list(&f, 1, PB_STRUCT);
        int lc = 0;
        pb_i(&f, &lc, 2, PB_I64, start);
        pb_field(&f, &lc, 3, PB_STRUCT);
        int lm = 0;
        pb_i(&f, &lm, 1, PB_I32, 6);
        pb_field(&f, &lm, 2, PB_LIST); pb_list(&f, 3, PB_I32); pb_zz(&f, 0); pb_zz(&f, 3); pb_zz(&f, 8);
        pb_field(&f, &lm, 3, PB_LIST); pb_list(&f, 1, PB_BIN); pb_bin(&f, "s", 1);
        pb_i(&f, &lm, 4, PB_I32, 0);
        pb_i(&f, &lm, 5, PB_I64, rows);
        pb_i(&f, &lm, 6, PB_I64, chunk);
        pb_i(&f, &lm, 7, PB_I64, chunk);
        pb_i(&f, &lm, 9, PB_I64, data_off);
        pb_i(&f, &lm, 11, PB_I64, start);
        pb_byte(&f, 0);   /* meta */
        pb_byte(&f, 0);   /* column chunk */
        pb_i(&f, &l, 2, PB_I64, chunk);
        pb_i(&f, &l, 3, PB_I64, rows);
        pb_byte(&f, 0);
    }
    pb_field(&f, &last, 6, PB_BIN); pb_bin(&f, "rayforce test", 13);
    pb_byte(&f, 0);
    uint32_t flen = (uint32_t)(f.n - (size_t)footer);
    pb_bytes(&f, &flen, 4); pb_bytes(&f, "PAR1", 4);
    bool ok = !f.bad && !dict.bad && !data.bad;
    FILE* out = ok ? fopen(path, "wb") : NULL;
    ok = out && fwrite(f.p, 1, f.n, out) == f.n;
    if (out && fclose(out)) ok = false;
    ray_sys_free(f.p); ray_sys_free(dict.p); ray_sys_free(data.p);
    return ok;
}
/* A stored dictionary page must decode to exactly the bytes it stores: a
 * header that declares fewer (here just the first entry's, with a count of
 * one, which would parse) or many more (up to the 64 MiB page limit, far
 * past the file) is an invalid page, for the read and for the direct and
 * grouped imports alike; the honest page reads. */
static test_result_t test_pq_dict_page_size(void) {
    pq_sym_env_clear();
    char src[160], dir[160], partial[200];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-dictsize-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-dictsize-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    int64_t tid = ray_sym_intern("SYM", 3);
    ray_t* types = ray_vec_from_raw(RAY_SYM, &tid, 1);
    const char* names[] = {"s"};
    const int64_t declared[3] = {4 + 5, (INT64_C(64) << 20) - 1, 4 + 5 + 4 + 19};
    const int64_t counts[3] = {1, 2, 2};
    for (int c = 0; c < 3; c++) {
        bool honest = c == 2;
        TEST_ASSERT_TRUE(pq_dict_file(src, declared[c], counts[c]));
        ray_t* t = ray_parquet_read(src, NULL);
        TEST_ASSERT_TRUE(t != NULL);
        TEST_ASSERT_TRUE(RAY_IS_ERR(t) != honest);
        if (honest) {
            size_t len; const char* text = ray_str_vec_get(ray_table_get_col_idx(t, 0), 5, &len);
            TEST_ASSERT_TRUE(len == 5 && !memcmp(text, "first", 5));
        }
        if (RAY_IS_ERR(t)) ray_error_free(t); else ray_release(t);
        for (int grouped = 0; grouped < 2; grouped++) {
            pq_set_symmode(grouped ? "grouped" : "direct");
            t = ray_parquet_splayed_typed(src, dir, types);
            pq_set_symmode(NULL);
            TEST_ASSERT_TRUE(t != NULL);
            TEST_ASSERT_TRUE(RAY_IS_ERR(t) != honest);
            if (RAY_IS_ERR(t)) ray_error_free(t); else ray_release(t);
            TEST_ASSERT_TRUE((access(dir, F_OK) == 0) == honest);
            pq_remove_native(dir, names, 1); pq_remove_native(partial, names, 1);
        }
    }
    ray_release(types); unlink(src);
    PASS();
}
/* Imports keep no memory after they end: the per-worker buffers the tasks
 * share (pages, dictionaries, symbol scratch) and each pass's state are
 * freed, direct and grouped.  One worker, so the calling thread's heap holds
 * it all; eight imports, the bytes allocated after the second and the last. */
static test_result_t test_pq_import_no_growth(void) {
    pq_sym_env_clear();
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("I64",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","y","s"};
    for (int grouped = 0; grouped < 2; grouped++) {
        size_t base = 0;
        for (int i = 0; i < 8; i++) {
            char dir[160]; snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-growth-%d-%d-%d", (int)getpid(), grouped, i);
            pq_set_symmode(grouped ? "grouped" : "direct");
            ray_t* res = ray_parquet_splayed_typed(FIX "row-groups.parquet", dir, types);
            pq_set_symmode(NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(res)); ray_release(res);
            pq_remove_native(dir, names, 3);
            ray_mem_stats_t st;
            ray_mem_stats(&st);
            if (i == 1) base = st.bytes_allocated;
            if (i == 7) TEST_ASSERT_TRUE(st.bytes_allocated <= base + (64u << 10));
        }
    }
    ray_release(types);
    ray_pool_destroy();
    PASS();
}
/* Edge inputs for the grouped import, each against the direct import of the
 * same file: strings past the 1 MB record buffer, a 4 MB read piece and the
 * 32 MB read-ahead (equal prefixes, told apart by their last bytes), a
 * string a later row group meets again only in the last one, row groups of
 * no rows, every string unique, every string the same; in every order, one
 * worker and four, one window or a window per row group, all of the hash or
 * none of it.  Every row's string, the vocabulary's size (no string lost
 * or twice), the null flag, and the files byte for byte across the worker
 * counts.  The default run is a subset (PQ_EDGE_FULL=1 runs all of it,
 * PQ_EDGE_ONLY=name one set, the PQ_EDGE_* knobs below change the runs). */
static char* pq_big;
static const char* pq_big_row(uint32_t len, char fill, int64_t key, uint32_t* out) {
    memset(pq_big, fill, len);
    char tail[24]; int n = snprintf(tail, sizeof(tail), "#%lld", (long long)key);
    if ((uint32_t)n <= len) memcpy(pq_big + len - (uint32_t)n, tail, (size_t)n);
    *out = len;
    return pq_big;
}
static const char* pq_synth_huge(int64_t r, char* buf, uint32_t* len) {
    switch (r % 8) {
    case 0: return NULL;
    case 1: *len = 0; return buf;
    case 2: return pq_big_row((1u << 20) - 4, 'b', (r / 8) % 2, len);   /* the 1 MB buffer exactly */
    case 3: return pq_big_row((1u << 20) - 3, 'b', (r / 8) % 3, len);   /* a byte past it */
    case 4: return pq_big_row((4u << 20) + 7, 'c', (r / 8) % 2, len);   /* past a 4 MB piece */
    case 5: *len = (uint32_t)snprintf(buf, 256, "k%lld", (long long)(r % 3)); return buf;
    case 6: return pq_big_row(300000, 'd', r % 5, len);
    default: return r == 15 ? pq_big_row(33u << 20, 'e', 7, len)          /* past the 32 MB read-ahead */
                            : pq_big_row((2u << 20) + 1, 'e', (r / 8) % 2, len);
    }
}
/* 4000 rows in 8 row groups of 500: forty strings of the first group met
 * again only in the last, ten of the fourth met again only in the last,
 * the rest unique. */
static const char* pq_synth_last(int64_t r, char* buf, uint32_t* len) {
    int64_t g = (r % 4000) / 500;
    int n;
    if ((g == 0 || g == 7) && r % 5 == 0) n = snprintf(buf, 256, "again-%lld", (long long)((r / 5) % 40));
    else if ((g == 3 || g == 7) && r % 7 == 1) n = snprintf(buf, 256, "mid-%lld", (long long)((r / 7) % 10));
    else if (r % 97 == 0) return NULL;
    else n = snprintf(buf, 256, "u%06lld", (long long)r);
    *len = (uint32_t)n;
    return buf;
}
static const char* pq_synth_same(int64_t r, char* buf, uint32_t* len) {
    (void)r;
    *len = (uint32_t)snprintf(buf, 256, "one and the same string");
    return buf;
}
/* pq_synth_check, telling the first row that differs. */
static bool pq_edge_check(const char* dir, int64_t rows, pq_synth_fn fn, const char* what, int nulls[2]) {
    char sym[200]; snprintf(sym, sizeof(sym), "%s/.sym", dir);
    ray_t* t = ray_read_splayed(dir, sym);
    if (!t || RAY_IS_ERR(t)) { fprintf(stderr, "  [%s] read failed\n", what); if (t) ray_error_free(t); return false; }
    bool ok = ray_table_nrows(t) == rows;
    if (!ok) fprintf(stderr, "  [%s] rows %lld != %lld\n", what, (long long)ray_table_nrows(t), (long long)rows);
    char buf[256];
    for (int c = 1; ok && c < PQ_SYNTH_COLS; c++) {
        ray_t* s = ray_table_get_col_idx(t, c);
        if (nulls) nulls[c - 1] = (s->attrs & RAY_ATTR_HAS_NULLS) != 0;
        for (int64_t r = 0; ok && r < rows; r++) {
            uint32_t len = 0;
            const char* want = pq_synth_row(fn, c, r, rows, buf, &len);
            if (!want) len = 0;
            ray_t* text = ray_sym_vec_cell(s, r);
            ok = text && ray_str_len(text) == len && (!len || !memcmp(ray_str_ptr(text), want, len));
            if (!ok) fprintf(stderr, "  [%s] col %d row %lld: want len %u, got len %lld pos %lld\n", what, c, (long long)r,
                             len, text ? (long long)ray_str_len(text) : -1LL,
                             (long long)ray_read_sym(ray_data(s), r, RAY_SYM, s->attrs));
        }
    }
    ray_release(t);
    return ok;
}
typedef struct { const char* name; pq_synth_fn fn; int64_t rows, groups; bool big, opt; } pq_edge_set;
static test_result_t test_pq_sym_grouped_edges(void) {
#if !defined(DEBUG)
    SKIP("window and hash knobs are debug-build only");
#else
    pq_sym_env_clear();
    pq_big = (char*)ray_sys_alloc((size_t)34 << 20);
    TEST_ASSERT_TRUE(pq_big != NULL);
    const pq_edge_set sets[] = {
        {"huge", pq_synth_huge, 24, 6, true, false},
        {"last", pq_synth_last, 4000, 8, false, false},
        {"zero", pq_synth_mixed, 7, 10, false, false},
        {"unique", pq_synth_long, 3000, 6, false, false},
        {"same", pq_synth_same, 3000, 5, false, false},
        {"zipf", pq_synth_zipf, 24000, 12, false, true},
        {"mixed", pq_synth_mixed, 20000, 9, false, true},
    };
    const char* e_arena = getenv("PQ_EDGE_ARENA");
    const char* e_bits = getenv("PQ_EDGE_BITS");
    const char* e_groups = getenv("PQ_EDGE_GROUPS");
    const char* e_window = getenv("PQ_EDGE_WINDOW");
    int e_cores = getenv("PQ_EDGE_CORES") ? atoi(getenv("PQ_EDGE_CORES")) : 4;
    bool full = getenv("PQ_EDGE_FULL") != NULL;
    const char* orders[] = {"rows","shards","freq","rowsflat"};
    const char* names[] = {"x","s","t"};
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    char src[160], dir[160], ref[160], a[220], b[220], what[160];
    int fails = 0;
    const char* only = getenv("PQ_EDGE_ONLY");
    for (size_t si = 0; si < sizeof(sets) / sizeof(sets[0]); si++) {
        const pq_edge_set* S = &sets[si];
        if (only ? strcmp(only, S->name) != 0 : S->opt) continue;   /* zipf, mixed: by name only (the other tests cover them) */
        snprintf(src, sizeof(src), "/tmp/rayforce-pq-edge-%d-%s.parquet", (int)getpid(), S->name);
        TEST_ASSERT_TRUE(pq_synth(src, S->rows, S->groups, S->fn));
        snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-edge-%d", (int)getpid());
        TEST_ASSERT_EQ_I(pq_pool(4), RAY_OK);
        pq_set_symmode("direct");
        ray_t* res = ray_parquet_splayed_typed(src, dir, types);
        pq_set_symmode(NULL);
        if (!res || RAY_IS_ERR(res)) {
            fprintf(stderr, "  [%s direct] import failed: %s\n", S->name, res ? ray_err_code(res) : "null");
            if (res) ray_error_free(res);
            fails++; unlink(src); continue;
        }
        ray_release(res);
        int dn[2] = {0, 0};
        snprintf(what, sizeof(what), "%s direct", S->name);
        if (!pq_edge_check(dir, S->rows, S->fn, what, dn)) fails++;
        int64_t words = pq_sym_count(dir);
        pq_remove_native(dir, names, 3);
        for (int o = 0; o < 4; o++) {
            for (int run = 0; run < 8; run++) {
                int cores = run & 1 ? e_cores : 1;
                const char* window = run & 2 ? (e_window ? e_window : "1") : NULL;
                const char* bits = run & 4 ? (e_bits ? e_bits : "0") : NULL;
                /* the default run: the big records in "rows" only and all of the
                 * hash; the truncated hash (every candidate a collision, slow
                 * under the sanitizers) in "rows" with one window.  PQ_EDGE_FULL=1: every
                 * order with every knob, the big records' collisions in "rows" */
                if (S->big && (full ? bits && o != 0 : bits || o != 0)) continue;
                if (!full && bits && (o != 0 || window)) continue;
                snprintf(what, sizeof(what), "%s %s cores=%d window=%s bits=%s", S->name, orders[o], cores,
                         window ? window : "-", bits ? bits : "-");
                TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
                snprintf(ref, sizeof(ref), "/tmp/rayforce-pq-edge-%d-ref-%d", (int)getpid(), run & 6);
                const char* out = cores == 1 ? ref : dir;
                pq_set_symmode("grouped");
                pq_set_env("RAY_PQ_SYM_ORDER", orders[o]);
                pq_set_env("RAY_PQ_SYM_WINDOW", window);
                pq_set_env("RAY_PQ_SYM_HASH_BITS", bits);
                pq_set_env("RAY_PQ_SYM_ARENA", e_arena);
                pq_set_env("RAY_PQ_SYM_GROUPS", e_groups);
                res = ray_parquet_splayed_typed(src, out, types);
                pq_sym_env_clear();
                if (!res || RAY_IS_ERR(res)) {
                    fprintf(stderr, "  [%s] import failed: %s\n", what, res ? ray_err_code(res) : "null");
                    if (res) ray_error_free(res);
                    char partial[240]; snprintf(partial, sizeof(partial), "%s.parquet-partial", out);
                    pq_remove_native(partial, names, 3);
                    fails++; continue;
                }
                TEST_ASSERT_EQ_I(res->i64, S->rows); ray_release(res);
                int gn[2] = {0, 0};
                if (!pq_edge_check(out, S->rows, S->fn, what, gn)) fails++;
                if (pq_sym_count(out) != words) {
                    fprintf(stderr, "  [%s] symbols %lld, direct %lld\n", what, (long long)pq_sym_count(out), (long long)words);
                    fails++;
                }
                if (gn[0] != dn[0] || gn[1] != dn[1]) {
                    fprintf(stderr, "  [%s] null flags %d/%d, direct %d/%d\n", what, gn[0], gn[1], dn[0], dn[1]);
                    fails++;
                }
                if (cores > 1) {
                    bool same = true;
                    snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
                    same = pq_same_file(a, b);
                    for (int c = 1; c < 3; c++) {
                        snprintf(a, sizeof(a), "%s/%s", ref, names[c]); snprintf(b, sizeof(b), "%s/%s", dir, names[c]);
                        if (!pq_same_file(a, b)) same = false;
                    }
                    if (!same) { fprintf(stderr, "  [%s] files differ from one worker's\n", what); fails++; }
                    pq_remove_native(dir, names, 3);
                    pq_remove_native(ref, names, 3);
                }
            }
        }
        unlink(src);
    }
    ray_pool_destroy();
    ray_sys_free(pq_big); pq_big = NULL;
    ray_release(types);
    TEST_ASSERT_EQ_I(fails, 0);
    PASS();
#endif
}
/* A file of many small row groups (4000 of 2 rows; PQ_RG_GROUPS), grouped
 * (or direct by PQ_RG_MODE): the grouped pass's staged arrays and its
 * workers' dedupe arenas cost memory by the rows, not a few pages a row
 * group or the generation limit (their bytes from the trace).  It prints
 * the process's committed-memory peak too, for the modes compared in
 * separate processes. */
static test_result_t test_pq_sym_grouped_rowgroups(void) {
    pq_sym_env_clear();
    const char* mode = getenv("PQ_RG_MODE");
    int64_t groups = getenv("PQ_RG_GROUPS") ? atoll(getenv("PQ_RG_GROUPS")) : 4000;
    int64_t rows = groups * 2;
    char src[160], dir[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-rg-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-rg-%d", (int)getpid());
    TEST_ASSERT_TRUE(pq_synth(src, rows, groups, pq_synth_mixed));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    TEST_ASSERT_EQ_I(pq_pool(4), RAY_OK);
    ray_mem_stats_t st0, st1;
    ray_mem_stats(&st0);
    pq_set_symmode(mode ? mode : "grouped");
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    ray_t* res = pq_traced_import(src, dir, types, trace, cap);
    pq_set_symmode(NULL);
    ray_mem_stats(&st1);
    bool failed = !res || RAY_IS_ERR(res);
    if (failed) fprintf(stderr, "  rowgroups: import failed: %s\n", res ? ray_err_code(res) : "null");
    if (res) { if (failed) ray_error_free(res); else ray_release(res); }
    int passes = 0;
    double stage = pq_trace_sum(trace, "stage_mb", &passes);
    double arena = pq_trace_sum(trace, "arena_mb", NULL);
    ray_sys_free(trace);
    long hwm = -1;
    FILE* f = fopen("/proc/self/status", "r");
    if (f) { char line[256]; while (fgets(line, sizeof(line), f)) if (!strncmp(line, "VmHWM:", 6)) hwm = atol(line + 6); fclose(f); }
    fprintf(stderr, "  rowgroups: mode=%s groups=%lld stage=%.1fMB arenas=%.1fMB in %d passes sys_peak=%.1fMB (before %.1fMB) VmHWM=%ldkB\n",
            mode ? mode : "grouped", (long long)groups, stage, arena, passes, (double)st1.sys_peak / 1048576.0,
            (double)st0.sys_peak / 1048576.0, hwm);
    TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_TRUE(pq_synth_check(dir, rows, pq_synth_mixed));
    if (!mode || !strcmp(mode, "grouped")) {
        TEST_ASSERT_EQ_I(passes, 2);
        TEST_ASSERT_TRUE(stage <= 2.0 * passes * (double)(groups > 4000 ? groups / 4000 : 1));
        TEST_ASSERT_TRUE(arena <= 1.0 * passes);   /* four workers, their arenas grown to the chunks' bytes */
    }
    pq_remove_native(dir, names, 3);
    ray_pool_destroy();
    ray_release(types); unlink(src);
    PASS();
}
/* The grouped import's failures keep no memory: each injected failure run
 * five times on one worker, the heap's and the system allocator's bytes
 * after the second run and after the last. */
static test_result_t test_pq_sym_grouped_fail_leaks(void) {
#if !defined(DEBUG)
    SKIP("failure injection is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], partial[200];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-fleak-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-fleak-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    TEST_ASSERT_TRUE(pq_synth(src, 20000, 9, pq_synth_mixed));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    const char* steps[] = {"r1","p2","load","reload","r2","write","settle","cancel",NULL};
    int bad = 0;
    for (int i = 0; i < 9; i++) {
        size_t base = 0, sbase = 0;
        for (int k = 0; k < 5; k++) {
            pq_set_symmode("grouped");
            pq_set_env("RAY_PQ_SYM_WINDOW", "4096");
            pq_set_env("RAY_PQ_SYM_HASH_BITS", "6");
            pq_set_env("RAY_PQ_SYM_INJECT", steps[i]);
            ray_t* res = ray_parquet_splayed_typed(src, dir, types);
            pq_sym_env_clear();
            bool failed = res && RAY_IS_ERR(res);
            if (res) { if (failed) ray_error_free(res); else ray_release(res); }
            ray_clear_interrupt();
            if (failed != (steps[i] != NULL)) bad++;
            pq_remove_native(partial, names, 3);
            pq_remove_native(dir, names, 3);
            ray_mem_stats_t st; ray_mem_stats(&st);
            if (k == 1) { base = st.bytes_allocated; sbase = st.sys_current; }
            if (k == 4) {
                fprintf(stderr, "  fail_leaks %-6s heap %+lld B sys %+lld B\n", steps[i] ? steps[i] : "none",
                        (long long)st.bytes_allocated - (long long)base, (long long)st.sys_current - (long long)sbase);
                /* a window's copy left behind is a few pages a run, the
                 * row groups' errors past the first a few hundred bytes */
                if (st.bytes_allocated > base + 512 || st.sys_current > sbase + (8u << 10)) bad++;
            }
        }
    }
    ray_pool_destroy();
    ray_release(types); unlink(src);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A cancelled import reports "cancel" whichever symbol import runs, the
 * interrupt seen in the first decode of the grouped one as well. */
static test_result_t test_pq_sym_grouped_cancel_code(void) {
    pq_sym_env_clear();
    char src[160], dir[160], partial[200];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-ccode-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pq-ccode-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    TEST_ASSERT_TRUE(pq_dict_file(src, 4 + 5 + 4 + 19, 2));
    int64_t tid = ray_sym_intern("SYM", 3);
    ray_t* types = ray_vec_from_raw(RAY_SYM, &tid, 1);
    const char* names[] = {"s"};
    char codes[2][32];
    for (int grouped = 0; grouped < 2; grouped++) {
        pq_set_symmode(grouped ? "grouped" : "direct");
        ray_request_interrupt();
        ray_t* res = ray_parquet_splayed_typed(src, dir, types);
        ray_clear_interrupt();
        pq_set_symmode(NULL);
        snprintf(codes[grouped], sizeof(codes[grouped]), "%s", res && RAY_IS_ERR(res) ? ray_err_code(res) : "ok");
        if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
        pq_remove_native(dir, names, 1); pq_remove_native(partial, names, 1);
    }
    fprintf(stderr, "  cancel codes: direct=%s grouped=%s\n", codes[0], codes[1]);
    ray_release(types); unlink(src);
    TEST_ASSERT_TRUE(!strcmp(codes[0], "cancel"));
    TEST_ASSERT_TRUE(!strcmp(codes[1], "cancel"));
    PASS();
}
/* Hash collisions resolved in parallel: with none of the hash's bits kept
 * every candidate of one length collides, and the records the collisions
 * add must not take positions in the order the workers reach them.  The
 * symbol file and the columns are the same bytes on one worker, two, four
 * and eight, run after run, in one window (candidates of other tasks
 * deferred) and in a window a task (compared at once), and in another
 * order of new positions. */
static test_result_t test_pq_sym_grouped_collision_determinism(void) {
#if !defined(DEBUG)
    SKIP("hash truncation is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[2][160], a[220], b[220];
    snprintf(src, sizeof(src), "/tmp/rayforce-pq-cdet-%d.parquet", (int)getpid());
    TEST_ASSERT_TRUE(pq_synth(src, 3000, 4, pq_synth_mixed));
    int64_t tids[] = {ray_sym_intern("I32",3), ray_sym_intern("SYM",3), ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    const char* names[] = {"x","s","t"};
    static const int cores[] = {1, 2, 4, 8, 4, 8};
    const int nruns = (int)(sizeof(cores) / sizeof(cores[0]));
    int differ = 0;
    for (int v = 0; v < 3; v++) {
        for (int i = 0; i < nruns; i++) {
            if (v && i && i != 3) continue;   /* the other variants: one worker and eight */
            int d = i ? 1 : 0;
            snprintf(dir[d], sizeof(dir[d]), "/tmp/rayforce-pq-cdet-%d-%d", (int)getpid(), d);
            TEST_ASSERT_EQ_I(pq_pool(cores[i]), RAY_OK);
            pq_set_symmode("grouped");
            pq_set_env("RAY_PQ_SYM_ORDER", v == 2 ? "shards" : "rows");
            pq_set_env("RAY_PQ_SYM_WINDOW", v == 1 ? "1" : NULL);
            pq_set_env("RAY_PQ_SYM_HASH_BITS", "0");
            ray_t* res = ray_parquet_splayed_typed(src, dir[d], types);
            pq_sym_env_clear();
            TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
            TEST_ASSERT_TRUE(pq_synth_check(dir[d], 3000, pq_synth_mixed));
            if (!i) continue;
            bool same = true;
            snprintf(a, sizeof(a), "%s/.sym", dir[0]); snprintf(b, sizeof(b), "%s/.sym", dir[1]);
            if (!pq_same_file(a, b)) same = false;
            for (int c = 1; c < 3; c++) {
                snprintf(a, sizeof(a), "%s/%s", dir[0], names[c]); snprintf(b, sizeof(b), "%s/%s", dir[1], names[c]);
                if (!pq_same_file(a, b)) same = false;
            }
            if (!same) { fprintf(stderr, "  variant %d, %d workers: files differ from one worker's\n", v, cores[i]); differ++; }
            pq_remove_native(dir[1], names, 3);
        }
        pq_remove_native(dir[0], names, 3);
    }
    ray_pool_destroy();
    ray_release(types); unlink(src);
    TEST_ASSERT_EQ_I(differ, 0);
    PASS();
#endif
}
/* ---- several symbol columns of their own make (pqx_synth) -----------------
 * x, a required INT32 (the row), then n optional BYTE_ARRAY (UTF8) columns,
 * each from its own function of the row and with its own pages: PQX_DICT a
 * dictionary page of the chunk's distinct strings and RLE ids; PQX_FALLBACK
 * the dictionary for the first half of the chunk's rows only and a PLAIN
 * page for the rest (a writer's fallback); PQX_SNAPPY every page of the
 * column snappy-compressed (a greedy LZ over 2-byte offsets). */
enum { PQX_DICT = 1, PQX_FALLBACK = 2, PQX_SNAPPY = 4, PQX_LATE = 8, PQX_NOSTATS = 16,
       PQX_LIESTATS = 32, PQX_ZSTD = 64, PQX_HUGERAW = 128, PQX_ZERORAW = 256, PQX_BADDICT = 512,
       PQX_FARDATA = 1024, PQX_INDICT = 2048, PQX_CRC = 4096, PQX_CRCFLIP = 8192 };   /* PQX_ZERORAW: total_uncompressed_size 0; PQX_BADDICT: the dictionary page encoded 3 */
                                             /* PQX_CRC: page checksums; PQX_CRCFLIP: and row group 1's dictionary
                                              * page with a byte flipped past its checksum (pqx_page, pqx_crc) */
                                             /* PQX_FARDATA: data_page_offset past the file's data; PQX_INDICT:
                                              * data_page_offset one byte into the dictionary page (skeptic round 4:
                                              * the decoder reads a chunk from its dictionary page for
                                              * total_compressed_size bytes and never uses it) */
                                             /* PQX_LATE: the fallback after 7/8 of the rows; PQX_NOSTATS: no encoding stats;
                                              * PQX_LIESTATS: stats that leave out the PLAIN pages; PQX_ZSTD: codec 6 in the
                                              * footer (pages stored as written); PQX_HUGERAW: total_uncompressed_size
                                              * INT64_MAX - 4096 in the footer (the pages as written) */
static int64_t pqx_rows = 1, pqx_groups = 1;   /* the file the functions describe */
/* pqx_empty >= 0: that row group has no rows (the others share them), its
 * chunks written as pyarrow writes a zero-row group's: a dictionary
 * column's an empty dictionary page and data_page_offset 0 */
static int64_t pqx_empty = -1;
static int64_t pqx_lo(int64_t rows, int64_t groups, int64_t g) {
    if (pqx_empty < 0 || groups < 2) return rows * g / groups;
    return rows * (g > pqx_empty ? g - 1 : g) / (groups - 1);
}
static void pqx_lit(pq_buf* o, const uint8_t* p, size_t n) {
    while (n) {
        size_t k = n < 65536 ? n : 65536;
        if (k <= 60) pb_byte(o, (uint8_t)((k - 1) << 2));
        else if (k <= 256) { pb_byte(o, 60 << 2); pb_byte(o, (uint8_t)(k - 1)); }
        else { pb_byte(o, 61 << 2); uint16_t x = (uint16_t)(k - 1); pb_bytes(o, &x, 2); }
        pb_bytes(o, p, k); p += k; n -= k;
    }
}
static void pqx_snappy(pq_buf* o, const uint8_t* s, size_t n) {
    pb_var(o, n);
    int32_t tab[4096];
    for (int i = 0; i < 4096; i++) tab[i] = -1;
    size_t lit = 0, i = 0;
    while (i + 4 <= n) {
        uint32_t v; memcpy(&v, s + i, 4);
        uint32_t h = (v * 2654435761u) >> 20;
        int32_t c = tab[h]; tab[h] = (int32_t)i;
        if (c >= 0 && i - (size_t)c <= 65535 && !memcmp(s + c, s + i, 4)) {
            pqx_lit(o, s + lit, i - lit);
            size_t len = 4;
            while (len < 64 && i + len < n && s[(size_t)c + len] == s[i + len]) len++;
            pb_byte(o, (uint8_t)(((len - 1) << 2) | 2));
            uint16_t off = (uint16_t)(i - (size_t)c); pb_bytes(o, &off, 2);
            i += len; lit = i;
        } else i++;
    }
    pqx_lit(o, s + lit, n - lit);
}
/* pqx_page's checksums (PQX_CRC, PQX_CRCFLIP): 0 none; 1 each page's
 * CRC-32 in its header (PageHeader 4); 2 that, and a dictionary page's
 * first string has one byte flipped after its checksum was taken (a page
 * gone bad on disk), the string it then reads kept in pqx_flipped. */
static int pqx_crc = 0;
static char pqx_flipped[256];
static uint32_t pqx_flipped_len = 0;
/* A page (kind 0 a v1 data page, 2 the dictionary): header and payload,
 * compressed when snappy; *raw adds what total_uncompressed_size counts. */
static void pqx_page(pq_buf* f, int kind, const pq_buf* body, int64_t count, int enc, bool snappy, int64_t* raw) {
    pq_buf z = {0};
    const pq_buf* pay = body;
    if (snappy) { pqx_snappy(&z, body->p, body->n); pay = &z; if (z.bad) f->bad = true; }
    size_t h0 = f->n;
    int last = 0;
    pb_i(f, &last, 1, PB_I32, kind);
    pb_i(f, &last, 2, PB_I32, (int64_t)body->n);
    pb_i(f, &last, 3, PB_I32, (int64_t)pay->n);
    if (pqx_crc) pb_i(f, &last, 4, PB_I32, (int32_t)ray_crc32(0, pay->p, pay->n));
    int l = 0;
    if (kind == 2) {
        pb_field(f, &last, 7, PB_STRUCT);
        pb_i(f, &l, 1, PB_I32, count); pb_i(f, &l, 2, PB_I32, enc); pb_byte(f, 0);
    } else {
        pb_field(f, &last, 5, PB_STRUCT);
        pb_i(f, &l, 1, PB_I32, count); pb_i(f, &l, 2, PB_I32, enc);
        pb_i(f, &l, 3, PB_I32, 3); pb_i(f, &l, 4, PB_I32, 3); pb_byte(f, 0);
    }
    pb_byte(f, 0);
    *raw += (int64_t)(f->n - h0) + (int64_t)body->n;
    size_t at = f->n;
    pb_bytes(f, pay->p, pay->n);
    if (pqx_crc == 2 && kind == 2 && !snappy && pay->n > 5 && !f->bad) {
        uint32_t len; memcpy(&len, pay->p, 4);
        if (len > 0 && len < sizeof(pqx_flipped) && 4 + (size_t)len <= pay->n) {
            f->p[at + 4] ^= 0x20;   /* the first string's first byte, its case flipped */
            memcpy(pqx_flipped, f->p + at + 4, len); pqx_flipped_len = len;
        }
    }
    ray_sys_free(z.p);
}
static uint32_t pqx_dict_id(const pq_buf* dict, const pq_buf* doff, const char* s, uint32_t len) {
    uint32_t nd = (uint32_t)(doff->n / 4), id = 0;
    for (; id < nd; id++) {
        uint32_t o; memcpy(&o, doff->p + id * 4, 4);
        uint32_t l; memcpy(&l, dict->p + o, 4);
        if (l == len && !memcmp(dict->p + o + 4, s, len)) break;
    }
    return id;
}
static bool pqx_synth(const char* path, int64_t rows, int64_t groups, int n, const char* const* names,
                      const pq_synth_fn* fns, const unsigned* flags) {
    pq_buf f = {0};
    int64_t nc = n + 1, cells = groups * nc;
    int64_t* start = (int64_t*)ray_sys_alloc((size_t)cells * sizeof(int64_t));
    int64_t* dpo = (int64_t*)ray_sys_alloc((size_t)cells * sizeof(int64_t));
    int64_t* dio = (int64_t*)ray_sys_alloc((size_t)cells * sizeof(int64_t));
    int64_t* stored = (int64_t*)ray_sys_alloc((size_t)cells * sizeof(int64_t));
    int64_t* rawt = (int64_t*)ray_sys_alloc((size_t)cells * sizeof(int64_t));
    if (!start || !dpo || !dio || !stored || !rawt) return false;
    pqx_rows = rows; pqx_groups = groups;
    pb_bytes(&f, "PAR1", 4);
    char buf[256];
    for (int64_t g = 0; g < groups; g++) {
        int64_t lo = pqx_lo(rows, groups, g), hi = pqx_lo(rows, groups, g + 1);
        for (int64_t c = 0; c < nc; c++) {
            int64_t i = g * nc + c;
            start[i] = (int64_t)f.n; rawt[i] = 0; dio[i] = -1;
            pqx_crc = 0;
            if (c == 0) {
                pq_buf v = {0};
                for (int64_t r = lo; r < hi; r++) { int32_t x = (int32_t)r; pb_bytes(&v, &x, 4); }
                dpo[i] = (int64_t)f.n;
                pqx_page(&f, 0, &v, hi - lo, 0, false, &rawt[i]);
                if (v.bad) f.bad = true;
                ray_sys_free(v.p);
                stored[i] = (int64_t)f.n - start[i];
                continue;
            }
            unsigned fl = flags[c - 1];
            pqx_crc = !(fl & PQX_CRC) ? 0 : (fl & PQX_CRCFLIP) && g == 1 ? 2 : 1;
            bool snappy = (fl & PQX_SNAPPY) != 0, dicted = (fl & (PQX_DICT | PQX_FALLBACK)) != 0;
            pq_synth_fn fn = fns[c - 1];
            int64_t mid = !dicted ? lo : (fl & PQX_FALLBACK) && hi - lo >= 2 ? lo + (hi - lo) * ((fl & PQX_LATE) ? 7 : 4) / 8 : hi;
            pq_buf dict = {0}, doff = {0};
            if (dicted) {
                for (int64_t r = lo; r < mid; r++) {
                    uint32_t len = 0;
                    const char* s = fn(r, buf, &len);
                    if (!s || pqx_dict_id(&dict, &doff, s, len) < doff.n / 4) continue;
                    uint32_t o = (uint32_t)dict.n; pb_bytes(&doff, &o, 4); pb_bytes(&dict, &len, 4); pb_bytes(&dict, s, len);
                }
                dio[i] = (int64_t)f.n;
                pqx_page(&f, 2, &dict, (int64_t)(doff.n / 4), (fl & PQX_BADDICT) ? 3 : 0, snappy, &rawt[i]);
            }
            dpo[i] = (int64_t)f.n;
            for (int part = 0; part < 2; part++) {
                int64_t a = part ? mid : lo, b = part ? hi : mid;
                if (b <= a) continue;
                bool ids = part == 0;
                pq_buf levels = {0}, values = {0}, body = {0};
                int64_t n8 = (b - a + 7) / 8;
                uint8_t bits = 0;
                pb_var(&levels, (uint64_t)(n8 * 2 + 1));
                uint32_t nd = (uint32_t)(doff.n / 4), bw = 1;
                while (bw < 32 && ((uint64_t)1 << bw) < nd) bw++;
                if (ids) pb_byte(&values, (uint8_t)bw);
                for (int64_t r = a; r < a + n8 * 8; r++) {
                    uint32_t len = 0;
                    const char* s = r < b ? fn(r, buf, &len) : NULL;
                    if (s) {
                        bits |= (uint8_t)(1u << ((r - a) & 7));
                        if (!ids) { pb_bytes(&values, &len, 4); pb_bytes(&values, s, len); }
                        else {
                            uint32_t id = pqx_dict_id(&dict, &doff, s, len);
                            pb_var(&values, 2); pb_bytes(&values, &id, (bw + 7) / 8);
                        }
                    }
                    if (((r - a) & 7) == 7) { pb_byte(&levels, bits); bits = 0; }
                }
                uint32_t ln = (uint32_t)levels.n;
                pb_bytes(&body, &ln, 4); pb_bytes(&body, levels.p, levels.n); pb_bytes(&body, values.p, values.n);
                if (levels.bad || values.bad || body.bad) f.bad = true;
                pqx_page(&f, 0, &body, b - a, ids ? 8 : 0, snappy, &rawt[i]);
                ray_sys_free(levels.p); ray_sys_free(values.p); ray_sys_free(body.p);
            }
            if (g == pqx_empty && dicted) dpo[i] = 0;   /* no data page: pyarrow writes offset 0 */
            if (dict.bad || doff.bad) f.bad = true;
            ray_sys_free(dict.p); ray_sys_free(doff.p);
            stored[i] = (int64_t)f.n - start[i];
        }
    }
    pqx_crc = 0;
    int64_t footer = (int64_t)f.n;
    int last = 0;
    pb_i(&f, &last, 1, PB_I32, 1);
    pb_field(&f, &last, 2, PB_LIST); pb_list(&f, (int)nc + 1, PB_STRUCT);
    { int l = 0; pb_i(&f, &l, 3, PB_I32, 0); pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, "schema", 6); pb_i(&f, &l, 5, PB_I32, nc); pb_byte(&f, 0); }
    { int l = 0; pb_i(&f, &l, 1, PB_I32, 1); pb_i(&f, &l, 3, PB_I32, 0); pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, "x", 1); pb_byte(&f, 0); }
    for (int c = 0; c < n; c++) {
        int l = 0;
        pb_i(&f, &l, 1, PB_I32, 6); pb_i(&f, &l, 3, PB_I32, 1);
        pb_field(&f, &l, 4, PB_BIN); pb_bin(&f, names[c], strlen(names[c])); pb_i(&f, &l, 6, PB_I32, 0); pb_byte(&f, 0);
    }
    pb_i(&f, &last, 3, PB_I64, rows);
    pb_field(&f, &last, 4, PB_LIST); pb_list(&f, (int)groups, PB_STRUCT);
    for (int64_t g = 0; g < groups; g++) {
        int64_t cnt = pqx_lo(rows, groups, g + 1) - pqx_lo(rows, groups, g), total = 0;
        int l = 0;
        pb_field(&f, &l, 1, PB_LIST); pb_list(&f, (int)nc, PB_STRUCT);
        for (int64_t c = 0; c < nc; c++) {
            int64_t i = g * nc + c;
            const char* nm = c ? names[c - 1] : "x";
            int lc = 0, lm = 0;
            pb_i(&f, &lc, 2, PB_I64, start[i]);
            pb_field(&f, &lc, 3, PB_STRUCT);
            pb_i(&f, &lm, 1, PB_I32, c ? 6 : 1);
            if (dio[i] >= 0) { pb_field(&f, &lm, 2, PB_LIST); pb_list(&f, 3, PB_I32); pb_zz(&f, 0); pb_zz(&f, 3); pb_zz(&f, 8); }
            else { pb_field(&f, &lm, 2, PB_LIST); pb_list(&f, 2, PB_I32); pb_zz(&f, 0); pb_zz(&f, 3); }
            pb_field(&f, &lm, 3, PB_LIST); pb_list(&f, 1, PB_BIN); pb_bin(&f, nm, strlen(nm));
            pb_i(&f, &lm, 4, PB_I32, c && (flags[c - 1] & PQX_ZSTD) ? 6 : c && (flags[c - 1] & PQX_SNAPPY) ? 1 : 0);
            pb_i(&f, &lm, 5, PB_I64, cnt);
            pb_i(&f, &lm, 6, PB_I64, c && (flags[c - 1] & PQX_HUGERAW) ? INT64_MAX - 4096
                                     : c && (flags[c - 1] & PQX_ZERORAW) ? 0 : rawt[i]);
            pb_i(&f, &lm, 7, PB_I64, stored[i]);
            pb_i(&f, &lm, 9, PB_I64, c && (flags[c - 1] & PQX_FARDATA) && dio[i] >= 0 ? footer + 4096
                                     : c && (flags[c - 1] & PQX_INDICT) && dio[i] >= 0 ? dio[i] + 1 : dpo[i]);
            if (dio[i] >= 0) pb_i(&f, &lm, 11, PB_I64, dio[i]);
            if (c && !(flags[c - 1] & PQX_NOSTATS))
                pb_enc_stats(&f, &lm, dio[i] >= 0, dio[i] >= 0,
                             (!(flags[c - 1] & PQX_DICT) || (flags[c - 1] & PQX_FALLBACK)) && !(flags[c - 1] & PQX_LIESTATS));
            pb_byte(&f, 0);
            pb_byte(&f, 0);
            total += stored[i];
        }
        pb_i(&f, &l, 2, PB_I64, total);
        pb_i(&f, &l, 3, PB_I64, cnt);
        pb_byte(&f, 0);
    }
    pb_field(&f, &last, 6, PB_BIN); pb_bin(&f, "rayforce test", 13);
    pb_byte(&f, 0);
    uint32_t flen = (uint32_t)(f.n - (size_t)footer);
    pb_bytes(&f, &flen, 4); pb_bytes(&f, "PAR1", 4);
    bool ok = !f.bad;
    FILE* out = ok ? fopen(path, "wb") : NULL;
    ok = out && fwrite(f.p, 1, f.n, out) == f.n;
    if (out && fclose(out)) ok = false;
    ray_sys_free(f.p);
    ray_sys_free(start); ray_sys_free(dpo); ray_sys_free(dio); ray_sys_free(stored); ray_sys_free(rawt);
    return ok;
}
/* An import of a pqx_synth file: its columns in the file's order, x the
 * row, every symbol row the function's string (a null and "" both the empty
 * symbol), the first difference told; nulls[c] gets column c's null flag. */
static bool pqx_check(const char* dir, int64_t rows, int n, const char* const* names, const pq_synth_fn* fns,
                      const char* what, int* nulls) {
    char sym[200]; snprintf(sym, sizeof(sym), "%s/.sym", dir);
    ray_t* t = ray_read_splayed(dir, sym);
    if (!t || RAY_IS_ERR(t)) { fprintf(stderr, "  [%s] read failed\n", what); if (t) ray_error_free(t); return false; }
    bool ok = ray_table_nrows(t) == rows && ray_table_ncols(t) == n + 1;
    if (!ok) fprintf(stderr, "  [%s] shape %lldx%lld\n", what, (long long)ray_table_nrows(t), (long long)ray_table_ncols(t));
    for (int c = 0; ok && c <= n; c++) {
        const char* nm = c ? names[c - 1] : "x";
        if (ray_table_col_name(t, c) != ray_sym_intern(nm, strlen(nm))) { fprintf(stderr, "  [%s] column %d is not %s\n", what, c, nm); ok = false; }
    }
    for (int64_t r = 0; ok && r < rows; r++)
        if (((int32_t*)ray_data(ray_table_get_col_idx(t, 0)))[r] != r) { fprintf(stderr, "  [%s] x row %lld\n", what, (long long)r); ok = false; }
    char buf[256];
    for (int c = 1; ok && c <= n; c++) {
        ray_t* s = ray_table_get_col_idx(t, c);
        if (nulls) nulls[c - 1] = (s->attrs & RAY_ATTR_HAS_NULLS) != 0;
        for (int64_t r = 0; ok && r < rows; r++) {
            uint32_t len = 0;
            const char* want = fns[c - 1](r, buf, &len);
            if (!want) len = 0;
            ray_t* text = ray_sym_vec_cell(s, r);
            ok = text && ray_str_len(text) == len && (!len || !memcmp(ray_str_ptr(text), want, len));
            if (!ok) fprintf(stderr, "  [%s] col %s row %lld: want %.*s, got %.*s\n", what, names[c - 1], (long long)r,
                             (int)len, want ? want : "", text ? (int)ray_str_len(text) : 0, text ? ray_str_ptr(text) : "");
        }
    }
    ray_release(t);
    return ok;
}
/* The columns of the mixed-column test: a low-cardinality one (dictionary,
 * snappy, long strings of one prefix), a high-cardinality one (PLAIN) that
 * repeats the first's every fifth row, one whose writer fell back to PLAIN
 * mid-chunk and that repeats the second's strings (unique ones too), a hot
 * key whose dictionary grows row group by row group and that meets the
 * first's strings, and long unique strings snappy compresses well; nulls
 * and "" in several. */
static const char* pqx_lowcard(int64_t r, char* buf, uint32_t* len) {
    if (r % 31 == 0) return NULL;
    if (r % 37 == 0) { *len = 0; return buf; }
    *len = (uint32_t)snprintf(buf, 256, "k%02lld-one-prefix-for-the-compressor", (long long)((r * 7) % 40));
    return buf;
}
static const char* pqx_highcard(int64_t r, char* buf, uint32_t* len) {
    if (r % 5 == 0) return pqx_lowcard(r + 1, buf, len);
    if (r % 41 == 0) { *len = 0; return buf; }
    if (r % 43 == 0) return NULL;
    *len = (uint32_t)snprintf(buf, 256, "u%06lld-high", (long long)r);
    return buf;
}
static const char* pqx_fallback(int64_t r, char* buf, uint32_t* len) {
    if (r % 4 == 0) return pqx_highcard(r + 7, buf, len);
    if (r % 53 == 0) return NULL;
    *len = (uint32_t)snprintf(buf, 256, "m%04lld", (long long)((r * 13) % 1500));
    return buf;
}
static const char* pqx_hot(int64_t r, char* buf, uint32_t* len) {
    int64_t g = r * pqx_groups / pqx_rows;
    if (r % 9 == 0) return pqx_lowcard(r, buf, len);
    if (r % 10 < 7) { *len = (uint32_t)snprintf(buf, 256, "HOT"); return buf; }
    *len = (uint32_t)snprintf(buf, 256, "d%lld", (long long)(r % (5 + 60 * g)));
    return buf;
}
static const char* pqx_unique(int64_t r, char* buf, uint32_t* len) {
    int n = snprintf(buf, 256, "e%08lld", (long long)r);
    memset(buf + n, 'z', 80);
    *len = (uint32_t)n + 80;
    return buf;
}
/* The footer's bound on what the direct import holds for a column
 * (pq_vocab_bounds, the trace's vocab_bytes) is an upper bound: at least
 * the symbol file the direct import writes, for a snappy dictionary of long
 * strings that share a prefix (its stored bytes far below its strings'), a
 * writer's PLAIN fallback over the chunk's last eighth of the rows (with
 * the encoding stats, and without them), and PLAIN pages only.  And not a
 * blanket one: a dictionary of forty strings with the stats saying no data
 * page holds values stays small however many rows its ids cover.  The
 * trace says which columns are dictionaries and ids only (dict_only), the
 * only ones a grouped import may still take direct. */
static const char* pqx_prefixed(int64_t r, char* buf, uint32_t* len) {
    memset(buf, 'P', 190);
    *len = 190 + (uint32_t)snprintf(buf + 190, 60, "%09lld", (long long)(r % 1000));
    return buf;
}
static const char* pqx_late(int64_t r, char* buf, uint32_t* len) {
    int64_t at = r % 8000;   /* the row in its row group of 8000 */
    if (at < 7000) { *len = (uint32_t)snprintf(buf, 256, "q%lld", (long long)(r % 10)); return buf; }
    buf[0] = (char)(33 + (r / 8836) % 94); buf[1] = (char)(33 + (r / 94) % 94); buf[2] = (char)(33 + r % 94);
    *len = 3;
    return buf;
}
/* A row in a thousand one of five strings, the rest null. */
static const char* pqx_sparse_fallback(int64_t r, char* buf, uint32_t* len) {
    if (r % 1000) return NULL;
    *len = (uint32_t)snprintf(buf, 256, "z%lld", (long long)((r / 1000) % 5));
    return buf;
}
static test_result_t test_pq_sym_vocab_bound(void) {
    pq_sym_env_clear();
    const char* nm[1] = {"p"};
    const char* fnames[2] = {"x","p"};
    char src[160], dir[160], sym[200];
    snprintf(src, sizeof(src), "/tmp/rayforce-pqv-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pqv-%d", (int)getpid());
    snprintf(sym, sizeof(sym), "%s/.sym", dir);
    int64_t tids[2] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 2);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    TEST_ASSERT_EQ_I(pq_pool(2), RAY_OK);
    static const struct { const char* what; pq_synth_fn fn; unsigned flags; int64_t rows; bool small; } cases[] = {
        {"snappy dictionary", pqx_prefixed, PQX_DICT | PQX_SNAPPY, 20000, false},
        {"late PLAIN fallback", pqx_late, PQX_FALLBACK | PQX_LATE, 16000, false},
        {"late PLAIN fallback, no stats", pqx_late, PQX_FALLBACK | PQX_LATE | PQX_NOSTATS, 16000, false},
        {"PLAIN", pqx_late, 0, 16000, false},
        {"forty strings", pqx_lowcard, PQX_DICT, 40000, true},
    };
    int bad = 0;
    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        pq_synth_fn fn[1] = {cases[k].fn};
        unsigned fl[1] = {cases[k].flags};
        TEST_ASSERT_TRUE(pqx_synth(src, cases[k].rows, 2, 1, nm, fn, fl));
        pq_set_symmode("direct");
        ray_t* res = pq_traced_import(src, dir, types, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        TEST_ASSERT_TRUE(pqx_check(dir, cases[k].rows, 1, nm, fn, cases[k].what, NULL));
        FILE* f = fopen(sym, "rb");
        long size = -1;
        if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
        double bound = pq_trace_sum(trace, "vocab_bytes", NULL);
        fprintf(stderr, "  %s: bound %.0f B, .sym %ld B, %lld symbols\n", cases[k].what, bound, size, (long long)pq_sym_count(dir));
        /* the records' bytes and the direct import's own per string (an
         * offset, a hash slot, at least 40 bytes) */
        if (size <= 0 || bound < (double)size + 40.0 * (double)pq_sym_count(dir)) bad++;
        if (cases[k].small && bound > 64 * 1024) bad++;
        /* dictionary pages and ids only: the stats saying no data page holds values */
        if (pq_trace_line_num(trace, "parquet symcol: col=p ", "dict_only") != (cases[k].flags & PQX_FALLBACK || !(cases[k].flags & PQX_DICT) ? 0 : 1)) {
            fprintf(stderr, "  %s: dict_only %.0f\n", cases[k].what, pq_trace_line_num(trace, "parquet symcol: col=p ", "dict_only"));
            bad++;
        }
        pq_remove_native(dir, fnames, 2);
    }
#if defined(DEBUG)
    /* A column whose chunks hold values in data pages (a fallback, though
     * nearly all its rows are null and its bound small) next to long unique
     * strings, with memory such that its bound is within a sixty-fourth and
     * the text past a quarter: it goes grouped all the same (a direct pass
     * gives such strings positions in the workers' order), the other too. */
    {
        const char* nm2[2] = {"p","q"};
        const char* fn2names[3] = {"x","p","q"};
        pq_synth_fn fn2[2] = {pqx_sparse_fallback, pqx_unique};
        unsigned fl2[2] = {PQX_FALLBACK, 0};
        int64_t tids3[3] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
        ray_t* types3 = ray_vec_from_raw(RAY_SYM, tids3, 3);
        TEST_ASSERT_TRUE(pqx_synth(src, 20000, 2, 2, nm2, fn2, fl2));
        pq_set_symmode("direct");
        ray_t* res = pq_traced_import(src, dir, types3, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        pq_remove_native(dir, fn2names, 3);
        double bp = pq_trace_line_num(trace, "parquet symcol: col=p ", "vocab_bytes");
        double text = pq_trace_line_num(trace, "parquet symcol: col=p ", "text_bytes") +
                      pq_trace_line_num(trace, "parquet symcol: col=q ", "text_bytes");
        char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)(bp * 64));
        if (bp <= 0 || bp * 64 / 4 >= text) { fprintf(stderr, "  sparse fallback: no memory size fits: p %.0f, text %.0f\n", bp, text); bad++; }
        pq_set_env("RAY_PQ_SYM_RAM", ramv);
        res = pq_traced_import(src, dir, types3, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        TEST_ASSERT_TRUE(pqx_check(dir, 20000, 2, nm2, fn2, "sparse fallback", NULL));
        bool p_grouped = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=grouped");
        fprintf(stderr, "  sparse fallback: bound %.0f B, p %s\n", bp, p_grouped ? "grouped" : "direct");
        if (!p_grouped) bad++;
        pq_remove_native(dir, fn2names, 3);
        ray_release(types3);
    }
#endif
    ray_sys_free(trace);
    ray_release(types); unlink(src);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
}
/* A hundred strings, and ten. */
static const char* pqx_card100(int64_t r, char* buf, uint32_t* len) {
    *len = (uint32_t)snprintf(buf, 256, "m%03lld", (long long)((r * 13) % 100));
    return buf;
}
static const char* pqx_card10(int64_t r, char* buf, uint32_t* len) {
    if (r % 17 == 0) return NULL;
    *len = (uint32_t)snprintf(buf, 256, "w%lld", (long long)((r * 3) % 10));
    return buf;
}
/* The column-by-column decision's caps, with the memory it assumes set from
 * the columns' bounds (RAY_PQ_SYM_RAM, debug builds): a dictionary column
 * whose bound is past a sixty-fourth of memory goes grouped though it would
 * fit the sixteenth the direct ones share; two dictionary columns within
 * both go direct, the whole import then direct (no grouped pass), their
 * dictionaries seeded in row-group order: one worker's files the same with
 * the row groups taken last to first. */
static test_result_t test_pq_sym_direct_caps(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160], a[240], b[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-pqcap-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pqcap-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-pqcap-%d-ref", (int)getpid());
    const char* fnames[3] = {"x","p","q"};
    const char* nm[2] = {"p","q"};
    int64_t tids[3] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int bad = 0;
    for (int k = 0; k < 2; k++) {
        /* 0: p a hundred strings, q a string a row; 1: p forty, q ten */
        pq_synth_fn fns[2] = {k ? pqx_lowcard : pqx_card100, k ? pqx_card10 : pqx_highcard};
        unsigned fl[2] = {PQX_DICT, k ? PQX_DICT : 0};
        int64_t rows = k ? 40000 : 20000;
        TEST_ASSERT_TRUE(pqx_synth(src, rows, 2, 2, nm, fns, fl));
        pq_set_symmode("direct");
        ray_t* res = pq_traced_import(src, dir, types, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        int64_t words = pq_sym_count(dir);
        pq_remove_native(dir, fnames, 3);
        double bp = pq_trace_line_num(trace, "parquet symcol: col=p ", "vocab_bytes");
        double bq = pq_trace_line_num(trace, "parquet symcol: col=q ", "vocab_bytes");
        double text = pq_trace_line_num(trace, "parquet symcol: col=p ", "text_bytes") +
                      pq_trace_line_num(trace, "parquet symcol: col=q ", "text_bytes");
        /* 0: p's bound a thirty-second of memory; 1: the larger one's a
         * sixty-fourth (the two within a sixteenth) */
        double big = bp > bq ? bp : bq;
        int64_t sram = k ? (int64_t)(big * 64) : (int64_t)(bp * 32);
        char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
        if (bp <= 0 || (double)sram / 4 >= text || (!k && bq <= (double)sram / 16)) {
            fprintf(stderr, "  caps %d: no memory size fits: p %.0f, q %.0f, text %.0f\n", k, bp, bq, text);
            bad++;
        }
        for (int run = 0; run < 2; run++) {
            const char* out = run ? dir : ref;
            pq_set_env("RAY_PQ_SYM_RAM", ramv);
            if (run) pq_set_env("RAY_PQ_DIRECT_REVERSE", "1");
            res = pq_traced_import(src, out, types, trace, cap);
            pq_sym_env_clear();
            TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
            TEST_ASSERT_TRUE(pqx_check(out, rows, 2, nm, fns, k ? "caps 1" : "caps 0", NULL));
            if (pq_sym_count(out) != words) { fprintf(stderr, "  caps %d: %lld symbols, direct %lld\n", k, (long long)pq_sym_count(out), (long long)words); bad++; }
            bool p_direct = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
            bool q_direct = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=direct");
            int passes = 0;
            (void)pq_trace_sum(trace, "rec_bytes", &passes);
            bool seeded = strstr(trace, "parquet symseed:") != NULL;
            fprintf(stderr, "  caps %d/%d: p %s, q %s, %d grouped passes%s\n", k, run, p_direct ? "direct" : "grouped",
                    q_direct ? "direct" : "grouped", passes, seeded ? ", seeded" : "");
            if (k ? !p_direct || !q_direct || passes != 0 || !seeded : p_direct || q_direct || passes != 2) bad++;
        }
        if (k) {
            bool same = true;
            snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
            if (!pq_same_file(a, b)) same = false;
            for (int c = 1; c < 3; c++) {
                snprintf(a, sizeof(a), "%s/%s", ref, fnames[c]); snprintf(b, sizeof(b), "%s/%s", dir, fnames[c]);
                if (!pq_same_file(a, b)) same = false;
            }
            if (!same) { fprintf(stderr, "  caps 1: files differ with the row groups reversed\n"); bad++; }
        }
        pq_remove_native(dir, fnames, 3); pq_remove_native(ref, fnames, 3);
        unlink(src);
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A file of no row groups, forced grouped: imported, no rows; a step
 * failing (resolve still runs, over no tasks) reports its error, publishes
 * nothing and keeps no memory, run after run. */
static test_result_t test_pq_sym_grouped_empty(void) {
#if !defined(DEBUG)
    SKIP("failure injection is a debug-build knob");
#else
    pq_sym_env_clear();
    const char* nm[1] = {"p"};
    const char* fn2[2] = {"x","p"};
    pq_synth_fn fn[1] = {pqx_unique};
    unsigned fl[1] = {0};
    char src[160], dir[160], partial[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-pqe-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pqe-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    TEST_ASSERT_TRUE(pqx_synth(src, 0, 0, 1, nm, fn, fl));
    int64_t tids[2] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 2);
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    const char* steps[] = {NULL, "p2"};
    int bad = 0;
    for (int i = 0; i < 2; i++) {
        size_t base = 0;
        for (int k = 0; k < 4; k++) {
            pq_set_symmode("grouped");
            pq_set_env("RAY_PQ_SYM_INJECT", steps[i]);
            ray_t* res = ray_parquet_splayed_typed(src, dir, types);
            pq_sym_env_clear();
            bool failed = !res || RAY_IS_ERR(res);
            bool published = access(dir, F_OK) == 0;
            if (failed != (steps[i] != NULL) || published == failed || (!failed && res->i64 != 0)) {
                fprintf(stderr, "  empty file, inject %s: %s, %s\n", steps[i] ? steps[i] : "none",
                        failed ? (res ? ray_err_code(res) : "null") : "imported", published ? "published" : "nothing published");
                bad++;
            }
            if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
            pq_remove_native(partial, fn2, 2); pq_remove_native(dir, fn2, 2);
            ray_mem_stats_t st; ray_mem_stats(&st);
            if (k == 1) base = st.bytes_allocated;
            if (k == 3 && st.bytes_allocated > base) {
                fprintf(stderr, "  empty file, inject %s: heap %+lld B\n", steps[i] ? steps[i] : "none",
                        (long long)st.bytes_allocated - (long long)base);
                bad++;
            }
        }
    }
    ray_pool_destroy();
    ray_release(types); unlink(src);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* Which way each symbol column went, from the trace's symcol lines: 'D' or
 * 'G' in the file's column order, '?' when the trace does not say. */
static void pqx_modes(const char* trace, int n, const char* const* names, char* out) {
    for (int c = 0; c < n; c++) {
        char line[64]; snprintf(line, sizeof(line), "parquet symcol: col=%s ", names[c]);
        out[c] = pq_trace_line_has(trace, line, " mode=direct") ? 'D' : pq_trace_line_has(trace, line, " mode=grouped") ? 'G' : '?';
    }
    out[n] = 0;
}
/* Direct and grouped columns side by side: five symbol columns (pqx_*)
 * whose strings meet across columns, imported with the memory the decision
 * assumes swept (RAY_PQ_SYM_RAM, debug builds) so that different sets go
 * direct, then with every order of new positions, hash groups, windows, a
 * small dedupe arena, none of the hash and column by column, on one worker
 * and four.  Every row as the function has it, the columns in the file's
 * order (the direct ones are imported first), the symbol file the direct
 * import's size (no string twice, none lost), the null flags the direct
 * import's, and every grouped record's writeback started by its window.
 * Mixed or forced grouped, the files do not depend on the workers.  Then each
 * injected failure of a mixed import, and repeated imports keeping no
 * memory.  The default run is a subset of the matrix; PQX_FULL=1 runs all
 * of it (PQX_ROWS, PQX_GROUPS, PQX_RAM, PQX_TRACE change it; PQX_DET runs a
 * probe of its own instead). */
static test_result_t test_pq_symgrp_mixed_columns(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    enum { N = 5 };
    const char* names[N] = {"b","a","e","d","c"};
    const pq_synth_fn fns[N] = {pqx_highcard, pqx_lowcard, pqx_unique, pqx_hot, pqx_fallback};
    const unsigned flags[N] = {0, PQX_DICT | PQX_SNAPPY, PQX_SNAPPY, PQX_DICT | PQX_SNAPPY, PQX_FALLBACK};
    const char* fnames[N + 1] = {"x","b","a","e","d","c"};
    const int64_t rows = getenv("PQX_ROWS") ? atoll(getenv("PQX_ROWS")) : 6000;
    const int64_t groups = getenv("PQX_GROUPS") ? atoll(getenv("PQX_GROUPS")) : 3;
    char src[160], dir[160], ref[160], what[200], a[240], b[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-pqx-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pqx-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-pqx-%d-ref", (int)getpid());
    TEST_ASSERT_TRUE(pqx_synth(src, rows, groups, N, names, fns, flags));
    int64_t tids[N + 1];
    tids[0] = ray_sym_intern("I32", 3);
    for (int c = 1; c <= N; c++) tids[c] = ray_sym_intern("SYM", 3);
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, N + 1);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    int bad = 0;
    /* the direct import: the vocabulary's size and the null flags */
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    pq_set_symmode("direct");
    ray_t* res = ray_parquet_splayed_typed(src, dir, types);
    pq_sym_env_clear();
    TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
    int dn[N] = {0}, gn[N] = {0};
    TEST_ASSERT_TRUE(pqx_check(dir, rows, N, names, fns, "direct", dn));
    int64_t words = pq_sym_count(dir);
    pq_remove_native(dir, fnames, N + 1);
    /* PQX_DET=1: whether the direct import's files depend on the workers
     * (one worker against four, three times), printed only */
    for (int k = 0; getenv("PQX_DET") && k < 3; k++) {
        for (int cores = 1; cores <= 4; cores += 3) {
            TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
            pq_set_symmode("direct");
            res = ray_parquet_splayed_typed(src, cores == 1 ? ref : dir, types);
            pq_sym_env_clear();
            TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        }
        bool same = true;
        snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
        if (!pq_same_file(a, b)) same = false;
        for (int c = 1; c <= N; c++) {
            snprintf(a, sizeof(a), "%s/%s", ref, fnames[c]); snprintf(b, sizeof(b), "%s/%s", dir, fnames[c]);
            if (!pq_same_file(a, b)) same = false;
        }
        fprintf(stderr, "  mixed_columns: direct import, 1 vs 4 workers, try %d: files %s\n", k, same ? "the same" : "differ");
        pq_remove_native(dir, fnames, N + 1); pq_remove_native(ref, fnames, N + 1);
    }
    /* the decision swept: a set of columns direct, the rest grouped (a pool
     * a worker count) */
    char seen[16][N + 1], mixram[16][32];
    int nmix = 0;
    int nseen = 0, mixed = 0;
    /* the default run sweeps on one worker; PQX_FULL=1 on one and four */
    bool full = getenv("PQX_FULL") != NULL;
    for (int cores = 1; cores <= (full ? 4 : 1); cores += 3) {
        TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
        for (int k = 0; k < 28; k++) {   /* half powers of two from 16 KB */
            int64_t m = (int64_t)16384 << (k / 2);
            char ram[32]; snprintf(ram, sizeof(ram), "%lld", (long long)(k & 1 ? m * 181 / 128 : m));
            pq_set_env("RAY_PQ_SYM_RAM", ram);
            res = pq_traced_import(src, dir, types, trace, cap);
            pq_sym_env_clear();
            char modes[N + 1]; pqx_modes(trace, N, names, modes);
            if (getenv("PQX_TRACE") && k == 0 && cores == 1) fprintf(stderr, "%s", trace);
            snprintf(what, sizeof(what), "ram=%s cores=%d modes=%s", ram, cores, modes);
            if (!res || RAY_IS_ERR(res)) {
                fprintf(stderr, "  [%s] import failed: %s\n", what, res ? ray_err_code(res) : "null");
                if (res) ray_error_free(res);
                char partial[240]; snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
                pq_remove_native(partial, fnames, N + 1);
                bad++; continue;
            }
            ray_release(res);
            if (!pqx_check(dir, rows, N, names, fns, what, gn)) bad++;
            if (pq_sym_count(dir) != words) { fprintf(stderr, "  [%s] symbols %lld, direct %lld\n", what, (long long)pq_sym_count(dir), (long long)words); bad++; }
            if (memcmp(gn, dn, sizeof(dn))) { fprintf(stderr, "  [%s] null flags differ from the direct import's\n", what); bad++; }
            int nd = 0, ng = 0;
            for (int c = 0; c < N; c++) { nd += modes[c] == 'D'; ng += modes[c] == 'G'; }
            if (nd && ng) mixed++;
            bool known = false;
            for (int s = 0; s < nseen; s++) if (!strcmp(seen[s], modes)) known = true;
            if (!known && nseen < 16) {
                memcpy(seen[nseen++], modes, N + 1);
                fprintf(stderr, "  mixed_columns: ram=%s modes %s (b a e d c)\n", ram, modes);
                if (nd && ng && nmix < 16) snprintf(mixram[nmix++], sizeof(mixram[0]), "%s", ram);
            }
            pq_remove_native(dir, fnames, N + 1);
        }
    }
    if (mixed < (full ? 4 : 2) || nseen < 3 || !nmix) { fprintf(stderr, "  mixed_columns: %d mixed imports, %d decisions\n", mixed, nseen); bad++; }
    if (!nmix) snprintf(mixram[nmix++], sizeof(mixram[0]), "%s", "1048576");
    /* a mixed decision with every knob of the grouped import, and forced
     * grouped: one worker's files, then four workers' the same bytes */
    const char* orders[] = {"rows","shards","freq","rowsflat"};
    for (int cores = 1; cores <= 4; cores += 3) {
        TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
        for (int run = 0; run < 48; run++) {
            int o = run % 4, knob = (run / 4) % 4;
            bool forced = run >= 32;
            /* the default run: neither none of the hash nor the small arena
             * with six bits of it (the collision tests have them), a window a
             * task with four groups forced grouped only; PQX_FULL=1 all of it */
            if (!full && (knob >= 2 || (!forced && knob == 1))) continue;
            /* the first mixed decision of the sweep, then the last */
            const char* ram_mixed = getenv("PQX_RAM") ? getenv("PQX_RAM") : mixram[run < 16 ? 0 : nmix - 1];
            const char* window = knob & 1 ? "1" : knob == 2 ? "4096" : NULL;
            const char* bits = knob == 2 ? "0" : knob == 3 ? "6" : NULL;
            const char* groupsv = knob & 1 ? "4" : NULL;
            const char* arena = knob == 3 ? "200" : NULL;
            /* one worker's files kept, a run each, for four workers' */
            snprintf(ref, sizeof(ref), "/tmp/rayforce-pqx-%d-ref-%d", (int)getpid(), run);
            const char* out = cores == 1 ? ref : dir;
            pq_set_env("RAY_PQ_SYM_RAM", ram_mixed);
            if (forced) pq_set_symmode("grouped");
            if (o == 1) pq_set_layout("1");   /* column by column, the direct ones too */
            pq_set_env("RAY_PQ_SYM_ORDER", orders[o]);
            pq_set_env("RAY_PQ_SYM_WINDOW", window);
            pq_set_env("RAY_PQ_SYM_HASH_BITS", bits);
            pq_set_env("RAY_PQ_SYM_GROUPS", groupsv);
            pq_set_env("RAY_PQ_SYM_ARENA", arena);
            res = pq_traced_import(src, out, types, trace, cap);
            pq_sym_env_clear();
            char modes[N + 1]; pqx_modes(trace, N, names, modes);
            snprintf(what, sizeof(what), "%s %s window=%s bits=%s groups=%s arena=%s cores=%d modes=%s", forced ? "forced" : "auto",
                     orders[o], window ? window : "-", bits ? bits : "-", groupsv ? groupsv : "-", arena ? arena : "-", cores, modes);
            if (!res || RAY_IS_ERR(res)) {
                fprintf(stderr, "  [%s] import failed: %s\n", what, res ? ray_err_code(res) : "null");
                if (res) ray_error_free(res);
                char partial[240]; snprintf(partial, sizeof(partial), "%s.parquet-partial", out);
                pq_remove_native(partial, fnames, N + 1);
                bad++; continue;
            }
            ray_release(res);
            if (!forced && (!strchr(modes, 'D') || !strchr(modes, 'G'))) { fprintf(stderr, "  [%s] not a mixed decision\n", what); bad++; }
            /* every record's writeback started by its window: exactly the
             * records' bytes where a window's records lie side by side */
            int gpasses = 0;
            double recb = pq_trace_sum(trace, "rec_bytes", &gpasses), wbb = pq_trace_sum(trace, "wb_bytes", NULL);
            if (gpasses < 2 || !(o == 0 || o == 3 ? wbb == recb : wbb >= recb)) {
                fprintf(stderr, "  [%s] %d grouped passes, rec_bytes %.0f, wb_bytes %.0f\n", what, gpasses, recb, wbb);
                bad++;
            }
            if (!pqx_check(out, rows, N, names, fns, what, gn)) bad++;
            if (pq_sym_count(out) != words) { fprintf(stderr, "  [%s] symbols %lld, direct %lld\n", what, (long long)pq_sym_count(out), (long long)words); bad++; }
            if (memcmp(gn, dn, sizeof(dn))) { fprintf(stderr, "  [%s] null flags differ from the direct import's\n", what); bad++; }
            if (cores > 1) {
                bool same = true;
                snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
                if (!pq_same_file(a, b)) same = false;
                for (int c = 1; c <= N; c++) {
                    snprintf(a, sizeof(a), "%s/%s", ref, fnames[c]); snprintf(b, sizeof(b), "%s/%s", dir, fnames[c]);
                    if (!pq_same_file(a, b)) same = false;
                }
                /* the direct passes of a mixed import too: their dictionaries
                 * are interned in row-group order first (pq_seed_dicts) */
                if (!same) { fprintf(stderr, "  [%s] files differ from one worker's\n", what); bad++; }
                pq_remove_native(dir, fnames, N + 1);
                pq_remove_native(ref, fnames, N + 1);
            }
        }
    }
    /* a mixed decision failing in each step of its grouped passes (a direct
     * pass before them): an error, nothing published, and on one worker no
     * memory kept, run after run (the heap's and the system allocator's
     * bytes after the second run and the last: the third, the fifth with
     * PQX_FULL=1); then the import itself as many times */
    const char* steps[] = {"r1","p2","load","reload","r2","write","settle","cancel",NULL};
    char partial[240]; snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    for (int i = 0; i < 9; i++) {
        size_t base = 0, sbase = 0;
        int runs = full ? 5 : 3;   /* the memory compared after the second and the last */
        for (int k = 0; k < runs; k++) {
            pq_set_env("RAY_PQ_SYM_RAM", mixram[nmix - 1]);
            pq_set_env("RAY_PQ_SYM_WINDOW", "1");
            pq_set_env("RAY_PQ_SYM_INJECT", steps[i]);
            res = ray_parquet_splayed_typed(src, dir, types);
            pq_sym_env_clear();
            bool failed = !res || RAY_IS_ERR(res);
            bool cancel = failed && res && !strcmp(ray_err_code(res), "cancel");
            if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
            ray_clear_interrupt();
            bool published = access(dir, F_OK) == 0;
            if (failed != (steps[i] != NULL) || (steps[i] && published) || cancel != (i == 7)) {
                fprintf(stderr, "  mixed_columns: inject %s: failed=%d cancel=%d published=%d\n", steps[i] ? steps[i] : "none",
                        failed, cancel, published);
                bad++;
            }
            pq_remove_native(partial, fnames, N + 1);
            pq_remove_native(dir, fnames, N + 1);
            ray_mem_stats_t st; ray_mem_stats(&st);
            if (k == 1) { base = st.bytes_allocated; sbase = st.sys_current; }
            if (k == runs - 1 && (st.bytes_allocated > base + 512 || st.sys_current > sbase + (8u << 10))) {
                fprintf(stderr, "  mixed_columns: inject %s: heap %+lld B sys %+lld B\n", steps[i] ? steps[i] : "none",
                        (long long)st.bytes_allocated - (long long)base, (long long)st.sys_current - (long long)sbase);
                bad++;
            }
        }
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types); unlink(src);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* Skeptic probes (pqio round 3).  The column files and .sym of two imports,
 * byte for byte; the ones that differ named. */
static bool sk_files_same(const char* ref, const char* dir, const char* const* fnames, int n) {
    char a[240], b[240];
    snprintf(a, sizeof(a), "%s/.sym", ref); snprintf(b, sizeof(b), "%s/.sym", dir);
    bool same = pq_same_file(a, b);
    if (!same) fprintf(stderr, "    differs: .sym\n");
    for (int c = 0; c < n; c++) {
        snprintf(a, sizeof(a), "%s/%s", ref, fnames[c]); snprintf(b, sizeof(b), "%s/%s", dir, fnames[c]);
        if (!pq_same_file(a, b)) { same = false; fprintf(stderr, "    differs: %s\n", fnames[c]); }
    }
    return same;
}
/* The memory (RAY_PQ_SYM_RAM) that makes p and q (dictionary columns) direct
 * and the import grouped: from a forced direct import's trace, sixty-four
 * times the larger bound, the text past a quarter of it.  0 when none fits. */
static int64_t sk_mixed_ram(const char* src, const char* dir, ray_t* types, const char* const* fnames, int nf,
                            char* trace, size_t cap, int64_t* words) {
    pq_set_symmode("direct");
    ray_t* res = pq_traced_import(src, dir, types, trace, cap);
    pq_sym_env_clear();
    bool ok = res && !RAY_IS_ERR(res);
    if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
    if (words) *words = ok ? pq_sym_count(dir) : -1;
    pq_remove_native(dir, (const char**)fnames, nf);
    double bp = pq_trace_line_num(trace, "parquet symcol: col=p ", "vocab_bytes");
    double bq = pq_trace_line_num(trace, "parquet symcol: col=q ", "vocab_bytes");
    double text = pq_trace_sum(trace, "text_bytes", NULL);
    /* an unbounded column (a chunk of no size: never direct) sets nothing */
    if (bp >= 0x1p62) bp = 0;
    if (bq >= 0x1p62) bq = 0;
    double big = bp > bq ? bp : bq;
    int64_t sram = (int64_t)(big * 64) + 1024;
    fprintf(stderr, "  sk: bounds p %.0f q %.0f text %.0f -> ram %lld\n", bp, bq, text, (long long)sram);
    return big > 0 && (double)sram / 4 < text ? sram : 0;
}
/* A mixed import (p, q dictionary columns direct and seeded, u grouped) and,
 * without u, an import every column of which qualifies (direct throughout,
 * seeded), on 1, 2, 4 and 8 workers, and with the direct passes' row groups
 * reversed on 1, 4 and 8: every run the same files as one worker's. */
static test_result_t test_pq_sk_mixed_workers(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk3w-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk3w-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-sk3w-%d-ref", (int)getpid());
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, pqx_card100, pqx_unique};
    unsigned fl[3] = {PQX_DICT | PQX_SNAPPY, PQX_DICT, 0};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    int bad = 0;
    for (int v = 0; v < 2; v++) {
    int n = v ? 2 : 3;
    /* all-direct: ten times the rows, the ids' text past a quarter of the
     * memory that keeps the dictionaries' bounds within a sixty-fourth */
    const int64_t rows = v ? 240000 : 24000;
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, n + 1);
    TEST_ASSERT_TRUE(pqx_synth(src, rows, 6, n, nm, fns, fl));
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int64_t words = 0;
    int64_t sram = sk_mixed_ram(src, dir, types, fnames, n + 1, trace, cap, &words);
    TEST_ASSERT_TRUE(sram > 0);
    char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
    static const struct { int cores; bool rev; } runs[] = {{1,false},{2,false},{4,false},{8,false},{1,true},{4,true},{8,true}};
    for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
        const char* out = k ? dir : ref;
        TEST_ASSERT_EQ_I(pq_pool(runs[k].cores), RAY_OK);
        pq_set_env("RAY_PQ_SYM_RAM", ramv);
        if (runs[k].rev) pq_set_env("RAY_PQ_DIRECT_REVERSE", "1");
        ray_t* res = pq_traced_import(src, out, types, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        char what[64]; snprintf(what, sizeof(what), "sk workers %s %d%s", v ? "all-direct" : "mixed", runs[k].cores, runs[k].rev ? " rev" : "");
        if (!pqx_check(out, rows, n, nm, fns, what, NULL)) bad++;
        bool pd = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
        bool qd = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=direct");
        bool ug = v || pq_trace_line_has(trace, "parquet symcol: col=u ", " mode=grouped");
        bool seeded = strstr(trace, "parquet symseed:") != NULL;
        bool same = !k || sk_files_same(ref, out, fnames + 1, n);
        fprintf(stderr, "  [%s] p %s q %s u %s%s, %lld symbols (direct %lld)%s\n", what, pd ? "D" : "G", qd ? "D" : "G",
                v ? "-" : ug ? "G" : "D", seeded ? " seeded" : "", (long long)pq_sym_count(out), (long long)words, same ? "" : ", FILES DIFFER");
        /* honest dictionaries: the seeding saw every string */
        if (pq_trace_line_num(trace, "parquet symdirect:", "seed_miss") != 0) bad++;
        if (!pd || !qd || !ug || !seeded || !same || pq_sym_count(out) != words) bad++;
        if (k) pq_remove_native(dir, fnames, n + 1);
    }
    pq_remove_native(ref, fnames, n + 1);
    unlink(src);
    ray_release(types);
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* Footers that would make a chunk look dictionary-only when it is not: 1, a
 * total_uncompressed_size of 0, a chunk of rows of no size: unsure, so q
 * goes grouped and every run writes one worker's files; 0, encoding stats
 * that leave out its PLAIN pages (a writer's fallback past the dictionary;
 * malformed, and only the pages themselves would tell): q goes direct and
 * seeded, and the trace counts the strings its passes add that the seeding
 * did not see (seed_miss, their positions the workers').  Values right in
 * both; whether the files differ is printed. */
static test_result_t test_pq_sk_lying_stats(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk3l-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk3l-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-sk3l-%d-ref", (int)getpid());
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 4);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    const int64_t rows = 48000;
    int bad = 0;
    for (int v = 0; v < 2; v++) {
    snprintf(src, sizeof(src), "/tmp/rayforce-sk3l-%d-%d.parquet", (int)getpid(), v);
    pq_synth_fn fns[3] = {pqx_lowcard, v ? pqx_hot : pqx_late, pqx_unique};   /* hot: dictionaries that differ by row group */
    unsigned fl[3] = {PQX_DICT | PQX_SNAPPY, v ? PQX_DICT | PQX_ZERORAW : PQX_FALLBACK | PQX_LIESTATS, 0};
    TEST_ASSERT_TRUE(pqx_synth(src, rows, 6, 3, nm, fns, fl));
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int64_t words = 0;
    int64_t sram = sk_mixed_ram(src, dir, types, fnames, 4, trace, cap, &words);
    double bq = pq_trace_line_num(trace, "parquet symcol: col=q ", "vocab_bytes");
    double donly = pq_trace_line_num(trace, "parquet symcol: col=q ", "dict_only");
    TEST_ASSERT_TRUE(sram > 0);
    char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
    static const struct { int cores; bool rev; } runs[] = {{1,false},{4,false},{8,false},{1,true}};
    int differ = 0;
    for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
        const char* out = k ? dir : ref;
        TEST_ASSERT_EQ_I(pq_pool(runs[k].cores), RAY_OK);
        pq_set_env("RAY_PQ_SYM_RAM", ramv);
        if (runs[k].rev) pq_set_env("RAY_PQ_DIRECT_REVERSE", "1");
        ray_t* res = pq_traced_import(src, out, types, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        char what[64]; snprintf(what, sizeof(what), "sk lying %s %d%s", v ? "raw=0" : "stats", runs[k].cores, runs[k].rev ? " rev" : "");
        if (!pqx_check(out, rows, 3, nm, fns, what, NULL)) bad++;
        bool qd = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=direct");
        bool pd = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
        double miss = pq_trace_line_num(trace, "parquet symdirect:", "seed_miss");
        bool same = !k || sk_files_same(ref, out, fnames + 1, 3);
        if (!same) differ++;
        char sym[200]; snprintf(sym, sizeof(sym), "%s/.sym", out);
        FILE* f = fopen(sym, "rb"); long size = -1;
        if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
        fprintf(stderr, "  [%s] q %s (dict_only %.0f, bound %.0f B), seed_miss %.0f, .sym %ld B, %lld symbols%s\n", what,
                qd ? "direct" : "grouped", donly, bq, miss, size, (long long)pq_sym_count(out), same ? "" : ", FILES DIFFER from one worker's");
        if (!pd || pq_sym_count(out) != words) bad++;
        if (v && (qd || donly != 0 || bq < 0x1p62 || miss != 0 || !same)) bad++;   /* unsure: unbounded */
        if (!v && (!qd || miss <= 0)) bad++;
        if (k) pq_remove_native(dir, fnames, 4);
    }
    fprintf(stderr, "  sk lying %s: %d of %d runs differ from one worker's files\n", v ? "raw=0" : "stats", differ,
            (int)(sizeof(runs) / sizeof(runs[0])) - 1);
    pq_remove_native(ref, fnames, 4);
    if (getenv("SK_KEEP")) fprintf(stderr, "  sk lying: kept %s (RAY_PQ_SYM_RAM=%s)\n", src, ramv); else unlink(src);
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A row group of no rows amid the others, written as pyarrow writes one (a
 * dictionary column's chunk an empty dictionary page and data_page_offset
 * 0): nothing to decode, so it neither unbounds a dictionary column nor
 * stops its seeding.  p and q go direct and seeded, u grouped, on one and
 * four workers alike; and a small file of the same make goes direct. */
static test_result_t test_pq_symgrp_empty_group(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-pqeg-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pqeg-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-pqeg-%d-ref", (int)getpid());
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, pqx_card100, pqx_unique};
    unsigned fl[3] = {PQX_DICT | PQX_SNAPPY, PQX_DICT, 0};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 4);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    const int64_t rows = 24000;
    pqx_empty = 3;
    bool made = pqx_synth(src, rows, 7, 3, nm, fns, fl);
    pqx_empty = -1;
    TEST_ASSERT_TRUE(made);
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int64_t words = 0;
    int64_t sram = sk_mixed_ram(src, dir, types, fnames, 4, trace, cap, &words);
    double op = pq_trace_line_num(trace, "parquet symcol: col=p ", "dict_only");
    double oq = pq_trace_line_num(trace, "parquet symcol: col=q ", "dict_only");
    TEST_ASSERT_TRUE(sram > 0);
    int bad = 0;
    if (op != 1 || oq != 1) { fprintf(stderr, "  empty group: dict_only p %.0f q %.0f\n", op, oq); bad++; }
    char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
    static const int cores[] = {1, 4};
    for (size_t k = 0; k < sizeof(cores) / sizeof(cores[0]); k++) {
        const char* out = k ? dir : ref;
        TEST_ASSERT_EQ_I(pq_pool(cores[k]), RAY_OK);
        pq_set_env("RAY_PQ_SYM_RAM", ramv);
        ray_error_clear();
        ray_t* res = pq_traced_import(src, out, types, trace, cap);
        pq_sym_env_clear();
        bool ok = res && !RAY_IS_ERR(res);
        if (!ok) fprintf(stderr, "  empty group %d: %s\n", cores[k], ray_error_msg() ? ray_error_msg() : "(no message)");
        if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
        TEST_ASSERT_TRUE(ok);
        char what[64]; snprintf(what, sizeof(what), "empty group %d", cores[k]);
        if (!pqx_check(out, rows, 3, nm, fns, what, NULL)) bad++;
        bool pd = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
        bool qd = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=direct");
        bool ug = pq_trace_line_has(trace, "parquet symcol: col=u ", " mode=grouped");
        bool seeded = strstr(trace, "parquet symseed:") != NULL;
        double miss = pq_trace_line_num(trace, "parquet symdirect:", "seed_miss");
        bool same = !k || sk_files_same(ref, out, fnames + 1, 3);
        fprintf(stderr, "  [%s] p %s q %s u %s%s, seed_miss %.0f, %lld symbols (direct %lld)%s\n", what, pd ? "D" : "G",
                qd ? "D" : "G", ug ? "G" : "D", seeded ? " seeded" : "", miss, (long long)pq_sym_count(out), (long long)words,
                same ? "" : ", FILES DIFFER");
        if (!pd || !qd || !ug || !seeded || miss != 0 || !same || pq_sym_count(out) != words) bad++;
        if (k) pq_remove_native(dir, fnames, 4);
    }
    pq_remove_native(ref, fnames, 4);
    /* nor does its chunk of u, PLAIN and of no size (no page), make a small
     * file's text unbounded: the import goes direct throughout */
    pqx_empty = 3;
    made = pqx_synth(src, 2400, 7, 3, nm, fns, fl);
    pqx_empty = -1;
    TEST_ASSERT_TRUE(made);
    pq_set_env("RAY_PQ_SYM_RAM", "16777216");
    ray_t* res = pq_traced_import(src, dir, types, trace, cap);
    pq_sym_env_clear();
    TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
    if (!pqx_check(dir, 2400, 3, nm, fns, "empty group, small", NULL)) bad++;
    bool ud = pq_trace_line_has(trace, "parquet symcol: col=u ", " mode=direct");
    if (!ud || strstr(trace, "parquet symseed:")) { fprintf(stderr, "  empty group, small: u %s\n", ud ? "direct, seeded" : "grouped"); bad++; }
    pq_remove_native(dir, fnames, 4);
    unlink(src);
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A mixed import over a degenerate hash (RAY_PQ_SYM_HASH_BITS=0: every
 * string of the grouped column in one hash group, each new one compared
 * with those before it): p and q direct and seeded, u grouped (PLAIN, few
 * distinct strings, which keeps the compares few under the sanitizers), on
 * one worker and on two: the same files, every row right. */
static test_result_t test_pq_symgrp_degenerate_hash(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-pqdh-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-pqdh-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-pqdh-%d-ref", (int)getpid());
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, pqx_card100, pqx_lowcard};
    unsigned fl[3] = {PQX_DICT | PQX_SNAPPY, PQX_DICT, 0};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 4);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    const int64_t rows = 18000;
    TEST_ASSERT_TRUE(pqx_synth(src, rows, 6, 3, nm, fns, fl));
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
    int64_t words = 0;
    int64_t sram = sk_mixed_ram(src, dir, types, fnames, 4, trace, cap, &words);
    TEST_ASSERT_TRUE(sram > 0);
    char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
    int bad = 0;
    for (int cores = 1; cores <= 2; cores++) {
        const char* out = cores == 1 ? ref : dir;
        TEST_ASSERT_EQ_I(pq_pool(cores), RAY_OK);
        pq_set_env("RAY_PQ_SYM_RAM", ramv);
        pq_set_env("RAY_PQ_SYM_HASH_BITS", "0");
        ray_t* res = pq_traced_import(src, out, types, trace, cap);
        pq_sym_env_clear();
        TEST_ASSERT_FALSE(!res || RAY_IS_ERR(res)); ray_release(res);
        char what[64]; snprintf(what, sizeof(what), "degenerate hash %d", cores);
        if (!pqx_check(out, rows, 3, nm, fns, what, NULL)) bad++;
        bool pd = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
        bool qd = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=direct");
        bool ug = pq_trace_line_has(trace, "parquet symcol: col=u ", " mode=grouped");
        bool seeded = strstr(trace, "parquet symseed:") != NULL;
        double collisions = pq_trace_sum(trace, "collisions", NULL);
        bool same = cores == 1 || sk_files_same(ref, out, fnames + 1, 3);
        fprintf(stderr, "  [%s] p %s q %s u %s%s, %.0f collisions, %lld symbols (direct %lld)%s\n", what, pd ? "D" : "G",
                qd ? "D" : "G", ug ? "G" : "D", seeded ? " seeded" : "", collisions, (long long)pq_sym_count(out),
                (long long)words, same ? "" : ", FILES DIFFER");
        if (!pd || !qd || !ug || !seeded || collisions <= 0 || !same || pq_sym_count(out) != words) bad++;
    }
    pq_remove_native(dir, fnames, 4);
    pq_remove_native(ref, fnames, 4);
    unlink(src);
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* Whether the file at `path` holds the n bytes `s`: 1, 0, -1 when it
 * cannot be read. */
static int pq_file_has(const char* path, const char* s, size_t n) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char buf[65536 + 256];
    size_t keep = 0, got;
    int found = 0;
    while (!found && n && (got = fread(buf + keep, 1, sizeof(buf) - keep, f)) > 0) {
        size_t have = keep + got;
        for (size_t i = 0; !found && i + n <= have; i++) found = !memcmp(buf + i, s, n);
        keep = have < n - 1 ? have : n - 1;   /* a match across the read boundary */
        memmove(buf, buf + have - keep, keep);
    }
    fclose(f);
    return found;
}
/* A dictionary column the reader cannot decode: a codec it does not take
 * (ZSTD, 6), a dictionary page of an encoding it does not know (3), or one
 * whose checksum (a file written with page CRCs) its bytes no longer match,
 * a byte of its first string flipped.  The import reports the decoder's own
 * reason and the column, forced direct and as a mixed import whose
 * dictionary column is seeded first (the error then the seeding's, the same
 * words); nothing is published, and the symbol file the failed import
 * leaves in its staging directory holds no string of the page gone bad. */
static test_result_t test_pq_sk_codec_message(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], partial[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk3c-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk3c-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, pqx_card100, pqx_unique};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 4);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    TEST_ASSERT_EQ_I(pq_pool(2), RAY_OK);
    int bad = 0;
    static const struct { unsigned flags; const char* want; } cases[] = {
        {PQX_DICT | PQX_ZSTD, "unsupported compression codec"},
        {PQX_DICT | PQX_BADDICT, "invalid dictionary page"},
        {PQX_DICT | PQX_CRC | PQX_CRCFLIP, "page checksum mismatch"},
    };
    for (size_t v = 0; v < sizeof(cases) / sizeof(cases[0]); v++) {
        unsigned fl[3] = {cases[v].flags, PQX_DICT, 0};
        TEST_ASSERT_TRUE(pqx_synth(src, 24000, 6, 3, nm, fns, fl));
        int64_t sram = sk_mixed_ram(src, dir, types, fnames, 4, trace, cap, NULL);
        pq_remove_native(partial, fnames, 4);
        TEST_ASSERT_TRUE(sram > 0);
        char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
        char first[256] = "";
        for (int run = 0; run < 2; run++) {
            if (run) pq_set_env("RAY_PQ_SYM_RAM", ramv); else pq_set_symmode("direct");
            ray_error_clear();
            ray_t* res = pq_traced_import(src, dir, types, trace, cap);
            pq_sym_env_clear();
            const char* msg = ray_error_msg();
            bool failed = !res || RAY_IS_ERR(res);
            bool pd = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
            fprintf(stderr, "  sk codec %zu %s: %s: %s%s\n", v, run ? "mixed" : "direct", failed ? "failed" : "imported",
                    msg ? msg : "(no message)", run ? "" : pd ? " (p direct)" : " (p grouped)");
            /* the mixed import fails in its seeding, before its symcol lines:
             * the seeding checks what it interns as the decoder would (no
             * page the pass then refuses is interned first: no symseed line) */
            if (!failed || !msg || !strstr(msg, cases[v].want) || !strstr(msg, "(column p)") || (!run && !pd)) bad++;
            if (!run && msg) snprintf(first, sizeof(first), "%s", msg);
            if (run && (!msg || strcmp(first, msg) || strstr(trace, "parquet symseed:"))) bad++;
            if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
            if (access(dir, F_OK) == 0) { fprintf(stderr, "  sk codec %zu: published\n", v); bad++; }
            if (cases[v].flags & PQX_CRCFLIP) {
                char sym[300]; snprintf(sym, sizeof(sym), "%s/.sym", partial);
                int has = pqx_flipped_len ? pq_file_has(sym, pqx_flipped, pqx_flipped_len) : 1;
                fprintf(stderr, "  sk codec %zu %s: the staged .sym %s\n", v, run ? "mixed" : "direct",
                        has < 0 ? "is not there" : has ? "HOLDS the bad page's string" : "holds no string of the bad page");
                if (has > 0) bad++;
            }
            pq_remove_native(partial, fnames, 4); pq_remove_native(dir, fnames, 4);
        }
        if (getenv("SK_KEEP")) fprintf(stderr, "  sk codec: kept %s\n", src); else unlink(src);
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A footer whose total_uncompressed_size is out of all proportion (INT64_MAX
 * - 4096) on a PLAIN symbol column, next to a dictionary column, imported
 * with memory that makes the import grouped: the dictionary column direct
 * and seeded, the PLAIN one grouped, every row right.  Run in a child (no
 * pool workers: fork-safe) whose exit says whether the import got through
 * the bound's arithmetic and the decision. */
static test_result_t test_pq_sk_huge_raw(void) {
#if !defined(DEBUG) || defined(RAY_OS_WINDOWS) || defined(RAY_OS_WASM)
    SKIP("the memory the decision assumes is a debug-build knob; fork");
#else
    pq_sym_env_clear();
    char src[160], dir[160], partial[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk3h-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk3h-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, pqx_highcard, pqx_unique};
    unsigned fl[3] = {PQX_DICT, PQX_HUGERAW, 0};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 4);
    /* five row groups: an odd count of wrapped per-chunk bounds sums negative */
    const int64_t rows = 20000;
    TEST_ASSERT_TRUE(pqx_synth(src, rows, 5, 3, nm, fns, fl));
    TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);   /* no worker threads */
    int bad = 0;
    for (int run = 0; run < 2; run++) {
        /* 0: grouped by the memory assumed (seeding, the direct caps);
         * 1: forced direct, the reference */
        fflush(stderr);
        pid_t pid = fork();
        TEST_ASSERT_TRUE(pid >= 0);
        if (!pid) {
            if (run) pq_set_symmode("direct"); else pq_set_env("RAY_PQ_SYM_RAM", "1073741824");
            ray_t* res = ray_parquet_splayed_typed(src, dir, types);
            bool ok = res && !RAY_IS_ERR(res);
            const char* msg = ray_error_msg();
            fprintf(stderr, "  sk huge raw (%s, child): %s%s%s\n", run ? "direct" : "auto, 1 GB", ok ? "imported" : "failed: ",
                    ok ? "" : (res ? ray_err_code(res) : "null"), ok ? "" : (msg ? msg : ""));
            if (ok) ok = pqx_check(dir, rows, 3, nm, fns, "sk huge raw", NULL);
            fflush(stderr);
            _exit(ok ? 0 : 1);
        }
        int status = 0;
        TEST_ASSERT_EQ_I(waitpid(pid, &status, 0), pid);
        bool clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        fprintf(stderr, "  sk huge raw %s: child %s %d\n", run ? "direct" : "auto", WIFEXITED(status) ? "exit" : "signal",
                WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
        if (!clean) bad++;
        (void)ray_test_rm_rf(partial); (void)ray_test_rm_rf(dir);
        (void)fnames;
    }
    if (getenv("SK_KEEP")) fprintf(stderr, "  sk huge raw: kept %s\n", src); else unlink(src);
    ray_pool_destroy();
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* Skeptic probes (pqio round 4).  A column whose chunks have no size
 * (total_uncompressed_size 0: unsure, unbounded by pq_chunk_vocab) beside a
 * small honest dictionary column, in a file whose sized text is within a
 * quarter of the memory assumed.  The unsure column should go grouped, as
 * the per-column rule has it, whatever the other columns' text: 0, q a
 * dictionary of a hundred strings, 16 MB assumed; 1 and 2, q PLAIN unique
 * 88-byte strings (100,000 rows, 8.8 MB of them), 1 MB assumed, with its
 * sizes honest (1: the control, grouped) and of no size (2). */
static test_result_t test_pq_sk4_unsure_small_text(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk4u-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk4u-%d", (int)getpid());
    const char* fnames[3] = {"x","p","q"};
    const char* nm[2] = {"p","q"};
    int64_t tids[3] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    TEST_ASSERT_EQ_I(pq_pool(2), RAY_OK);
    int bad = 0;
    for (int v = 0; v < 3; v++) {
        pq_synth_fn fns[2] = {pqx_lowcard, v ? pqx_unique : pqx_card100};
        unsigned fl[2] = {PQX_DICT | PQX_SNAPPY, v == 0 ? PQX_DICT | PQX_ZERORAW : v == 1 ? 0u : (unsigned)PQX_ZERORAW};
        const int64_t rows = v ? 100000 : 24000;
        const char* ram = v ? "1048576" : "16777216";
        TEST_ASSERT_TRUE(pqx_synth(src, rows, 6, 2, nm, fns, fl));
        pq_set_env("RAY_PQ_SYM_RAM", ram);
        ray_t* res = pq_traced_import(src, dir, types, trace, cap);
        pq_sym_env_clear();
        bool ok = res && !RAY_IS_ERR(res);
        if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
        char what[32]; snprintf(what, sizeof(what), "sk4 unsure %d", v);
        if (!ok || !pqx_check(dir, rows, 2, nm, fns, what, NULL)) bad++;
        bool qg = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=grouped");
        double vq = pq_trace_line_num(trace, "parquet symcol: col=q ", "vocab_bytes");
        double dq = pq_trace_line_num(trace, "parquet symcol: col=q ", "dict_only");
        double tq = pq_trace_line_num(trace, "parquet symcol: col=q ", "text_bytes");
        double text = pq_trace_sum(trace, "text_bytes", NULL);
        double tab = pq_trace_line_num(trace, "parquet symimp:", "tab_mb");
        double rec = pq_trace_line_num(trace, "parquet symimp:", "rec_mb");
        fprintf(stderr, "  [%s] text %.0f B (q %.0f) of %s B assumed, q vocab %.3g B dict_only %.0f: q %s;"
                " import dictionary tab_mb %.1f rec_mb %.1f\n", what, text, tq, ram, vq, dq,
                qg ? "grouped" : "DIRECT", tab, rec);
        if (!qg) bad++;
        pq_remove_native(dir, fnames, 3);
        if (v == 2 && getenv("SK_KEEP")) fprintf(stderr, "  sk4 unsure: kept %s\n", src); else unlink(src);
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A mixed import (p, q direct and seeded, u grouped) and an all-direct one
 * (no u) over a file with a zero-row group (pyarrow's layout) and a row
 * group whose q chunk holds nulls only (an empty dictionary page, ids of no
 * value): on 1, 2, 4 and 8 workers, the direct passes reversed on 1, 4 and
 * 8; every run the same files as one worker's, every row right, the
 * seeding seeing every string. */
static int64_t sk4_null_lo = 0, sk4_null_hi = 0;
static const char* sk4_nullgrp(int64_t r, char* buf, uint32_t* len) {
    if (r >= sk4_null_lo && r < sk4_null_hi) return NULL;
    return pqx_card100(r, buf, len);
}
static test_result_t test_pq_sk4_null_group_workers(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], ref[160];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk4n-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk4n-%d", (int)getpid());
    snprintf(ref, sizeof(ref), "/tmp/rayforce-sk4n-%d-ref", (int)getpid());
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, sk4_nullgrp, pqx_unique};
    unsigned fl[3] = {PQX_DICT | PQX_SNAPPY, PQX_DICT, 0};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    int bad = 0;
    for (int v = 0; v < 2; v++) {
        int n = v ? 2 : 3;
        const int64_t rows = v ? 240000 : 24000;
        /* seven row groups, the fourth (3) of no rows: group 2 is rows [2/6, 3/6) */
        sk4_null_lo = rows * 2 / 6; sk4_null_hi = rows * 3 / 6;
        ray_t* types = ray_vec_from_raw(RAY_SYM, tids, n + 1);
        pqx_empty = 3;
        bool made = pqx_synth(src, rows, 7, n, nm, fns, fl);
        pqx_empty = -1;
        TEST_ASSERT_TRUE(made);
        TEST_ASSERT_EQ_I(pq_pool(1), RAY_OK);
        int64_t words = 0;
        int64_t sram = sk_mixed_ram(src, dir, types, fnames, n + 1, trace, cap, &words);
        TEST_ASSERT_TRUE(sram > 0);
        char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
        static const struct { int cores; bool rev; } runs[] = {{1,false},{2,false},{4,false},{8,false},{1,true},{4,true},{8,true}};
        for (size_t k = 0; k < sizeof(runs) / sizeof(runs[0]); k++) {
            const char* out = k ? dir : ref;
            TEST_ASSERT_EQ_I(pq_pool(runs[k].cores), RAY_OK);
            pq_set_env("RAY_PQ_SYM_RAM", ramv);
            if (runs[k].rev) pq_set_env("RAY_PQ_DIRECT_REVERSE", "1");
            ray_t* res = pq_traced_import(src, out, types, trace, cap);
            pq_sym_env_clear();
            bool ok = res && !RAY_IS_ERR(res);
            if (!ok) fprintf(stderr, "  sk4 null group: %s\n", ray_error_msg() ? ray_error_msg() : "(no message)");
            if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
            TEST_ASSERT_TRUE(ok);
            char what[64]; snprintf(what, sizeof(what), "sk4 null group %s %d%s", v ? "all-direct" : "mixed", runs[k].cores, runs[k].rev ? " rev" : "");
            int nulls[3] = {0, 0, 0};
            if (!pqx_check(out, rows, n, nm, fns, what, nulls)) bad++;
            bool pd = pq_trace_line_has(trace, "parquet symcol: col=p ", " mode=direct");
            bool qd = pq_trace_line_has(trace, "parquet symcol: col=q ", " mode=direct");
            bool ug = v || pq_trace_line_has(trace, "parquet symcol: col=u ", " mode=grouped");
            bool seeded = strstr(trace, "parquet symseed:") != NULL;
            double miss = pq_trace_line_num(trace, "parquet symdirect:", "seed_miss");
            bool same = !k || sk_files_same(ref, out, fnames + 1, n);
            /* the grouped pass's first reservation gave the shard tables back
             * (no lookup in flight there): the trace's index bytes are none */
            double ikb = v ? 0 : pq_trace_line_num(trace, "parquet symgrp:", "imp_index_kb");
            fprintf(stderr, "  [%s] p %s q %s u %s%s, seed_miss %.0f, q nulls %d, imp_index_kb %.0f, %lld symbols (direct %lld)%s\n", what,
                    pd ? "D" : "G", qd ? "D" : "G", v ? "-" : ug ? "G" : "D", seeded ? " seeded" : "", miss, nulls[1], ikb,
                    (long long)pq_sym_count(out), (long long)words, same ? "" : ", FILES DIFFER");
            if (!pd || !qd || !ug || !seeded || miss != 0 || !same || !nulls[1] || ikb != 0 || pq_sym_count(out) != words) bad++;
            if (k) pq_remove_native(dir, fnames, n + 1);
        }
        pq_remove_native(ref, fnames, n + 1);
        unlink(src);
        ray_release(types);
    }
    ray_pool_destroy();
    ray_sys_free(trace);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* A dictionary column (p) whose data_page_offset does not point at its
 * first data page: past the file's data (0), one byte into its dictionary
 * page (1).  The decoder reads a chunk from its dictionary page for
 * total_compressed_size bytes and never uses that offset, so the forced
 * direct import takes the file.  The mixed import (p direct and seeded, u
 * grouped) should take it too, or fail as the decoder would. */
static test_result_t test_pq_sk4_data_offset(void) {
#if !defined(DEBUG)
    SKIP("the memory the decision assumes is a debug-build knob");
#else
    pq_sym_env_clear();
    char src[160], dir[160], partial[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk4d-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk4d-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    const char* fnames[4] = {"x","p","q","u"};
    const char* nm[3] = {"p","q","u"};
    pq_synth_fn fns[3] = {pqx_lowcard, pqx_card100, pqx_unique};
    int64_t tids[4] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 4);
    size_t cap = (size_t)1 << 16;
    char* trace = (char*)ray_sys_alloc(cap);
    TEST_ASSERT_TRUE(trace != NULL);
    TEST_ASSERT_EQ_I(pq_pool(2), RAY_OK);
    int bad = 0;
    for (int v = 0; v < 2; v++) {
        unsigned fl[3] = {PQX_DICT | (v ? (unsigned)PQX_INDICT : (unsigned)PQX_FARDATA), PQX_DICT, 0};
        const int64_t rows = 24000;
        TEST_ASSERT_TRUE(pqx_synth(src, rows, 6, 3, nm, fns, fl));
        /* forced direct: no seeding */
        pq_set_symmode("direct");
        ray_error_clear();
        ray_t* res = pq_traced_import(src, dir, types, trace, cap);
        pq_sym_env_clear();
        bool dok = res && !RAY_IS_ERR(res);
        char dmsg[256]; snprintf(dmsg, sizeof(dmsg), "%s", dok ? "imported" : ray_error_msg() ? ray_error_msg() : "(no message)");
        if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
        if (dok && !pqx_check(dir, rows, 3, nm, fns, "sk4 data offset direct", NULL)) dok = false;
        double bp = pq_trace_line_num(trace, "parquet symcol: col=p ", "vocab_bytes");
        double bq = pq_trace_line_num(trace, "parquet symcol: col=q ", "vocab_bytes");
        double text = pq_trace_sum(trace, "text_bytes", NULL);
        pq_remove_native(dir, fnames, 4); pq_remove_native(partial, fnames, 4);
        /* the memory that makes p and q direct and the import grouped (sk_mixed_ram) */
        double big = bp > bq ? bp : bq;
        int64_t sram = (int64_t)(big * 64) + 1024;
        char ramv[32]; snprintf(ramv, sizeof(ramv), "%lld", (long long)sram);
        pq_set_env("RAY_PQ_SYM_RAM", ramv);
        ray_error_clear();
        res = pq_traced_import(src, dir, types, trace, cap);
        pq_sym_env_clear();
        bool mok = res && !RAY_IS_ERR(res);
        char mmsg[256]; snprintf(mmsg, sizeof(mmsg), "%s", mok ? "imported" : ray_error_msg() ? ray_error_msg() : "(no message)");
        if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
        bool seeded = strstr(trace, "parquet symseed:") != NULL;
        fprintf(stderr, "  [sk4 data offset %s] text %.0f ram %lld; direct: %s; mixed%s: %s\n", v ? "in dict" : "past data", text,
                (long long)sram, dmsg, seeded ? " (seeded)" : "", mmsg);
        if (dok && !mok) bad++;
        pq_remove_native(dir, fnames, 4); pq_remove_native(partial, fnames, 4);
        if (getenv("SK_KEEP")) {
            char kept[200]; snprintf(kept, sizeof(kept), "/tmp/rayforce-sk4d-%d-%d.parquet", (int)getpid(), v);
            if (rename(src, kept) == 0) fprintf(stderr, "  sk4 data offset: kept %s (RAY_PQ_SYM_RAM=%s)\n", kept, ramv);
        }
    }
    unlink(src);
    ray_pool_destroy();
    ray_sys_free(trace);
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
#endif
}
/* The same undecodable dictionary page (encoding 3, PQX_BADDICT) in p, the
 * import forced direct and forced grouped: both fail, and the grouped
 * message should carry the decoder's reason and the column as the direct
 * one does.  So too a codec the reader does not take (ZSTD), which the
 * chunk's checks before its pages refuse. */
static test_result_t test_pq_sk4_grouped_message(void) {
    pq_sym_env_clear();
    char src[160], dir[160], partial[240];
    snprintf(src, sizeof(src), "/tmp/rayforce-sk4g-%d.parquet", (int)getpid());
    snprintf(dir, sizeof(dir), "/tmp/rayforce-sk4g-%d", (int)getpid());
    snprintf(partial, sizeof(partial), "%s.parquet-partial", dir);
    const char* fnames[3] = {"x","p","q"};
    const char* nm[2] = {"p","q"};
    pq_synth_fn fns[2] = {pqx_lowcard, pqx_card100};
    int64_t tids[3] = {ray_sym_intern("I32", 3), ray_sym_intern("SYM", 3), ray_sym_intern("SYM", 3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM, tids, 3);
    TEST_ASSERT_EQ_I(pq_pool(2), RAY_OK);
    int bad = 0;
    static const unsigned cases[] = {PQX_DICT | PQX_BADDICT, PQX_DICT | PQX_ZSTD};
    for (size_t v = 0; v < sizeof(cases) / sizeof(cases[0]); v++) {
    unsigned fl[2] = {cases[v], PQX_DICT};
    TEST_ASSERT_TRUE(pqx_synth(src, 12000, 4, 2, nm, fns, fl));
    char msgs[2][256];
    for (int run = 0; run < 2; run++) {
        pq_set_symmode(run ? "grouped" : "direct");
        ray_error_clear();
        ray_t* res = ray_parquet_splayed_typed(src, dir, types);
        pq_sym_env_clear();
        bool failed = !res || RAY_IS_ERR(res);
        const char* msg = ray_error_msg();
        snprintf(msgs[run], sizeof(msgs[run]), "%s", msg ? msg : "(no message)");
        fprintf(stderr, "  [sk4 message %zu %s] %s: %s\n", v, run ? "grouped" : "direct", failed ? "failed" : "imported", msgs[run]);
        if (!failed || !strstr(msgs[run], "(column p)")) bad++;
        if (res) { if (RAY_IS_ERR(res)) ray_error_free(res); else ray_release(res); }
        pq_remove_native(dir, fnames, 3); pq_remove_native(partial, fnames, 3);
    }
    if (strcmp(msgs[0], msgs[1]) != 0) bad++;
    }
    unlink(src);
    ray_pool_destroy();
    ray_release(types);
    TEST_ASSERT_EQ_I(bad, 0);
    PASS();
}
const test_entry_t parquet_entries[] = {
    {"parquet/sk4_grouped_message",test_pq_sk4_grouped_message,pq_setup,pq_teardown},
    {"parquet/sk4_unsure_small_text",test_pq_sk4_unsure_small_text,pq_setup,pq_teardown},
    {"parquet/sk4_data_offset",test_pq_sk4_data_offset,pq_setup,pq_teardown},
    {"parquet/sk4_null_group_workers",test_pq_sk4_null_group_workers,pq_setup,pq_teardown},
    {"parquet/sk_huge_raw",test_pq_sk_huge_raw,pq_setup,pq_teardown},
    {"parquet/sk_mixed_workers",test_pq_sk_mixed_workers,pq_setup,pq_teardown},
    {"parquet/sk_lying_stats",test_pq_sk_lying_stats,pq_setup,pq_teardown},
    {"parquet/symgrp_empty_group",test_pq_symgrp_empty_group,pq_setup,pq_teardown},
    {"parquet/symgrp_degenerate_hash",test_pq_symgrp_degenerate_hash,pq_setup,pq_teardown},
    {"parquet/sk_codec_message",test_pq_sk_codec_message,pq_setup,pq_teardown},
    {"parquet/small_stack",test_pq_small_stack,NULL,NULL},
    {"parquet/parted_symbols",test_pq_parted_symbols,pq_setup,pq_teardown},
    {"parquet/snappy",test_pq_snappy,pq_setup,pq_teardown},
    {"parquet/batch_matrix",test_pq_matrix,pq_setup,pq_teardown},
    {"parquet/read_metadata",test_pq_read_meta,pq_setup,pq_teardown},
    {"parquet/corrupt",test_pq_corrupt,pq_setup,pq_teardown},
    {"parquet/native",test_pq_native,pq_setup,pq_teardown},
    {"parquet/indexes",test_pq_indexes,pq_setup,pq_teardown},
    {"parquet/bloom",test_pq_bloom,pq_setup,pq_teardown},
    {"parquet/native_edges",test_pq_native_edges,pq_setup,pq_teardown},
    {"parquet/native_dictionary",test_pq_native_dictionary,pq_setup,pq_teardown},
    {"parquet/group_native",test_pq_group_native,pq_setup,pq_teardown},
    {"parquet/sym_grouped",test_pq_sym_grouped,pq_setup,pq_teardown},
    {"parquet/sym_grouped_collisions",test_pq_sym_grouped_collisions,pq_setup,pq_teardown},
    {"parquet/sym_grouped_layout",test_pq_sym_grouped_layout,pq_setup,pq_teardown},
    {"parquet/sym_mixed_modes",test_pq_sym_mixed_modes,pq_setup,pq_teardown},
    {"parquet/sym_grouped_failures",test_pq_sym_grouped_failures,pq_setup,pq_teardown},
    {"parquet/dict_page_size",test_pq_dict_page_size,pq_setup,pq_teardown},
    {"parquet/import_no_growth",test_pq_import_no_growth,pq_setup,pq_teardown},
    {"parquet/symgrp_collision_determinism",test_pq_sym_grouped_collision_determinism,pq_setup,pq_teardown},
    {"parquet/symgrp_cancel_code",test_pq_sym_grouped_cancel_code,pq_setup,pq_teardown},
    {"parquet/symgrp_edges",test_pq_sym_grouped_edges,pq_setup,pq_teardown},
    {"parquet/symgrp_fail_leaks",test_pq_sym_grouped_fail_leaks,pq_setup,pq_teardown},
    {"parquet/symgrp_rowgroups",test_pq_sym_grouped_rowgroups,pq_setup,pq_teardown},
    {"parquet/symgrp_mixed_columns",test_pq_symgrp_mixed_columns,pq_setup,pq_teardown},
    {"parquet/sym_vocab_bound",test_pq_sym_vocab_bound,pq_setup,pq_teardown},
    {"parquet/sym_grouped_empty",test_pq_sym_grouped_empty,pq_setup,pq_teardown},
    {"parquet/sym_direct_caps",test_pq_sym_direct_caps,pq_setup,pq_teardown},
#if defined(__linux__)
    {"parquet/chunk_flush_error",test_pq_chunk_flush_error,pq_setup,pq_teardown},
#endif
    {"parquet/splayed_inline_indexes",test_pq_splayed_inline_indexes,pq_setup,pq_teardown},
    {"parquet/parallel",test_pq_parallel,pq_setup,pq_teardown},
    {"parquet/range",test_pq_range,pq_setup,pq_teardown},
    {NULL,NULL,NULL,NULL}
};
