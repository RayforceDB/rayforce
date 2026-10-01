#ifndef RAY_PARQUET_H
#define RAY_PARQUET_H

#include <rayforce.h>

/* Flat Parquet reader. All implementation and codecs are dependency-free.
 * A reader owns its mapping and reusable page buffers. next() returns an
 * OWNED table, independent of the reader; NULL means EOF. Errors are owned.
 * columns is a borrowed SYM vector (NULL = all). No file data is decoded at
 * open. batch_rows bounds rows per returned table, not compressed page size.
 * Public reader calls stay on the opening thread. next() may dispatch
 * independent column cursors to the native pool and joins before returning. */
typedef struct ray_parquet ray_parquet_t;
ray_t* ray_parquet_open(const char* path, ray_t* columns, int64_t batch_rows,
                        ray_parquet_t** out);
ray_t* ray_parquet_next(ray_parquet_t* reader);
/* Inclusive integer range; applies exact filtering as well as conservative
 * row-group/page pruning and equality Bloom probes. Configure once, before
 * next(). Filter column need not be projected. Signed integer columns only. */
ray_t* ray_parquet_range(ray_parquet_t* reader, int64_t column, int64_t lo, int64_t hi);
int64_t ray_parquet_groups_skipped(const ray_parquet_t* reader);
/* Counters are cumulative; pages count physical column data pages bypassed
 * during interval pruning. bloom_skipped is a subset of groups_skipped. */
int64_t ray_parquet_pages_skipped(const ray_parquet_t* reader);
int64_t ray_parquet_bloom_skipped(const ray_parquet_t* reader);
int64_t ray_parquet_parallel_batches(const ray_parquet_t* reader);
void   ray_parquet_close(ray_parquet_t* reader);
ray_t* ray_parquet_metadata(const char* path);
/* options: NULL, projected SYM column names, or {columns, types, range, strict}.
 * types covers the full file schema. SYM text uses a private refcounted domain. */
ray_t* ray_parquet_read(const char* path, ray_t* options);
/* Native imports return an owned row-count atom/error and publish a NEW
 * destination after writes complete. Typed schemas are positional SYM vectors;
 * SYM encodes text, integer widths are checked, and raw integer temporal values
 * require explicit UNIX_DATE/UNIX_SECONDS/UNIX_MILLIS/UNIX_MICROS/UNIX_NANOS.
 * Annotated DATE/TIMESTAMP retain their declared units. NULL uses file types.
 * An options dictionary may supply types, rows (1..1048576) and strict. Strict
 * mode rejects decoded null/value collisions and invalid annotated UTF-8. */
ray_t* ray_parquet_splayed_typed(const char* path, const char* dir, ray_t* types);
ray_t* ray_parquet_parted_typed(const char* path, const char* root, const char* table, ray_t* types);
ray_t* ray_parquet_splayed(const char* path, const char* dir);
ray_t* ray_parquet_parted(const char* path, const char* root, const char* table);

/* Raw Snappy block (Parquet framing). Exact input/output lengths required.
 * Exposed internally for codec regression tests. */
bool ray_parquet_snappy(const uint8_t* src, size_t len, uint8_t* dst, size_t size);

/* Returns NULL for a non-Parquet source; otherwise an owned table/error.
 * complete=true means a streaming aggregate already answered the query. */
ray_t* ray_parquet_select_source(ray_t* source, ray_t* query, bool* complete);
ray_t* ray_parquet_scan_fn(ray_t** args, int64_t n);
ray_t* ray_parquet_read_fn(ray_t** args, int64_t n);
ray_t* ray_parquet_metadata_fn(ray_t* path);
ray_t* ray_parquet_splayed_fn(ray_t** args, int64_t n);
ray_t* ray_parquet_parted_fn(ray_t** args, int64_t n);
ray_t* ray_parquet_each_fn(ray_t** args, int64_t n);

#endif
