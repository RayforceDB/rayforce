/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE. */
#include "test.h"
#include "io/parquet.h"
#include "core/pool.h"
#include "mem/heap.h"
#include "table/sym.h"
#include "store/part.h"
#include "store/splay.h"
#include "ops/idxop.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#if !defined(RAY_OS_WINDOWS) && !defined(RAY_OS_WASM)
#include <pthread.h>
#endif

static void pq_setup(void) { ray_heap_init(); (void)ray_sym_init(); }
static void pq_teardown(void) { ray_sym_destroy(); ray_heap_destroy(); }
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
        ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(workers),RAY_OK);
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
static test_result_t test_pq_group_native(void) {
    const char* names[] = {"x","y","s"};
    int64_t tids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,tids,3);
    for (int cores = 1; cores <= 8; cores *= 2) {
        ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(cores),RAY_OK);
        for (int sym = 0; sym < 2; sym++) {
            char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-groups-%d-%d-%d",(int)getpid(),cores,sym);
            ray_t* result = ray_parquet_splayed_typed(FIX "row-groups.parquet",dir,sym ? types : NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(result)); TEST_ASSERT_EQ_I(result->i64,44009); ray_release(result);
            char domain[200]; snprintf(domain,sizeof(domain),"%s/.sym",dir);
            ray_t* table = ray_read_splayed(dir,sym ? domain : NULL);
            TEST_ASSERT_FALSE(RAY_IS_ERR(table)); TEST_ASSERT_EQ_I(ray_table_nrows(table),44009);
            ray_t* strings = ray_table_get_col_idx(table,2);
            TEST_ASSERT_EQ_I(strings->type,sym ? RAY_SYM : RAY_STR);
            for (int64_t i = 0; i < 44009; i++) {
                TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(table,0)))[i],i);
                TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(table,1)))[i],i%13 ? i*3 : NULL_I64);
                char expected[64]; int n = snprintf(expected,sizeof(expected),"long pooled string row %lld",(long long)i);
                size_t len; const char* text;
                if (sym) { ray_t* atom = ray_sym_vec_cell(strings,i); text = ray_str_ptr(atom); len = ray_str_len(atom); }
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
    char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-unix-%d",(int)getpid());
    ray_t* result = ray_parquet_splayed_typed(FIX "unix.parquet",dir,types);
    TEST_ASSERT_FALSE(RAY_IS_ERR(result)); ray_release(result); ray_release(types);
    char domain[200]; snprintf(domain,sizeof(domain),"%s/.sym",dir);
    ray_t* table = ray_read_splayed(dir,domain); TEST_ASSERT_FALSE(RAY_IS_ERR(table));
    TEST_ASSERT_EQ_I(ray_table_get_col_idx(table,0)->type,RAY_DATE);
    TEST_ASSERT_EQ_I(((int32_t*)ray_data(ray_table_get_col_idx(table,0)))[1],1);
    TEST_ASSERT_EQ_I(ray_table_get_col_idx(table,1)->type,RAY_TIMESTAMP);
    TEST_ASSERT_EQ_I(((int64_t*)ray_data(ray_table_get_col_idx(table,1)))[1],1000000000);
    ray_release(table); pq_remove_native(dir,unames,3);
    PASS();
}
static test_result_t test_pq_native_edges(void) {
    int64_t ids[] = {ray_sym_intern("I32",3),ray_sym_intern("I64",3),ray_sym_intern("SYM",3)};
    ray_t* types = ray_vec_from_raw(RAY_SYM,ids,3);
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(4),RAY_OK);
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
    char dir[160]; snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-rle-%d",(int)getpid());
    t = ray_parquet_splayed_typed(FIX "rle-runs.parquet",dir,types);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); ray_release(t); ray_release(types);
    char sym[200]; snprintf(sym,sizeof(sym),"%s/.sym",dir);
    t = ray_read_splayed(dir,sym); TEST_ASSERT_FALSE(RAY_IS_ERR(t));
    for (int i = 0; i < 24597; i++) {
        ray_t* text = ray_sym_vec_cell(ray_table_get_col_idx(t,2),i);
        TEST_ASSERT_EQ_I(ray_str_len(text),i%8199 < 4099 ? 24 : 0);
    }
    ray_release(t);
    const char* names[] = {"x","y","s"}; pq_remove_native(dir,names,3);
    int64_t empty_ids[] = {ray_sym_intern("I32",3),ray_sym_intern("SYM",3),ray_sym_intern("I16",3),
        ray_sym_intern("I64",3),ray_sym_intern("DATE",4),ray_sym_intern("TIMESTAMP",9),ray_sym_intern("F64",3)};
    types = ray_vec_from_raw(RAY_SYM,empty_ids,7);
    snprintf(dir,sizeof(dir),"/tmp/rayforce-pq-empty-native-%d",(int)getpid());
    t = ray_parquet_splayed_typed(FIX "empty.parquet",dir,types);
    TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(t->i64,0); ray_release(t);
    snprintf(sym,sizeof(sym),"%s/.sym",dir);
    t = ray_read_splayed(dir,sym); TEST_ASSERT_FALSE(RAY_IS_ERR(t)); TEST_ASSERT_EQ_I(ray_table_nrows(t),0); ray_release(t);
    const char* enames[] = {"x","name","flag","wide","day","ts","f"}; pq_remove_native(dir,enames,7);
    /* A corrupt worker must join the others and leave the final name absent. */
    t = ray_parquet_splayed_typed(FIX "projection.parquet",dir,types);
    TEST_ASSERT_TRUE(RAY_IS_ERR(t)); ray_release(t); TEST_ASSERT_TRUE(access(dir,F_OK) != 0);
    char partial[200]; snprintf(partial,sizeof(partial),"%s.parquet-partial",dir); pq_remove_native(partial,enames,7);
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
    ray_pool_destroy(); TEST_ASSERT_EQ_I(ray_pool_init_total(8),RAY_OK);
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
const test_entry_t parquet_entries[] = {
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
    {"parquet/group_native",test_pq_group_native,pq_setup,pq_teardown},
    {"parquet/parallel",test_pq_parallel,pq_setup,pq_teardown},
    {"parquet/range",test_pq_range,pq_setup,pq_teardown},
    {NULL,NULL,NULL,NULL}
};
