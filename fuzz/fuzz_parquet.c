/* Copyright (c) 2026 Anton Kundenko. MIT license; see LICENSE.
 * Native footer/page/encoding/index parser. Seeds are ordinary Parquet files.
 * Bound the amount of output consumed so forged row counts cannot turn one
 * input into unbounded materialization. The reader still checks each page it
 * visits, including dictionary IDs, levels and decoded lengths. */
#include "common.h"
#include "io/parquet.h"

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    ray_fuzz_init();
    if (size < 12 || size > 1024*1024) return 0;
    char path[64];
    int fd = ray_fuzz_memfd(data,size,path,sizeof(path));
    if (fd < 0) return 0;
    ray_parquet_t* reader = NULL;
    ray_t* err = ray_parquet_open(path,NULL,257,&reader);
    if (err) ray_release(err);
    else {
        /* Exercise indexed pruning too. A missing/unsupported x column is
         * an expected error and leaves the cursor available for plain scan. */
        if (size&1) {
            err = ray_parquet_range(reader,ray_sym_intern("x",1),2,7);
            if (err) ray_release(err);
        }
        for (int batch = 0; batch < 256; batch++) {
            ray_t* table = ray_parquet_next(reader);
            if (!table) break;
            bool failed = RAY_IS_ERR(table);
            ray_release(table);
            if (failed) break;
        }
    }
    ray_parquet_close(reader);
    close(fd);
    return 0;
}
