/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.

 *   Permission is hereby granted, free of charge, to any person obtaining a copy
 *   of this software and associated documentation files (the "Software"), to deal
 *   in the Software without restriction, including without limitation the rights
 *   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *   copies of the Software, and to permit persons to whom the Software is
 *   furnished to do so, subject to the following conditions:

 *   The above copyright notice and this permission notice shall be included in all
 *   copies or substantial portions of the Software.

 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 */

#ifndef RAY_H
#define RAY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== Semantic Versioning ===== */

#define RAY_VERSION_MAJOR 2
#define RAY_VERSION_MINOR 1
#define RAY_VERSION_PATCH 0

/* Packed version number: 0xMMmmpp (MM=major, mm=minor, pp=patch) */
#define RAY_VERSION_NUMBER \
    ((RAY_VERSION_MAJOR * 10000) + (RAY_VERSION_MINOR * 100) + RAY_VERSION_PATCH)

/* Compile-time version check: true if lib version >= (major, minor, patch) */
#define RAY_VERSION_AT_LEAST(major, minor, patch) \
    (RAY_VERSION_NUMBER >= ((major) * 10000 + (minor) * 100 + (patch)))

/* Runtime version query */
int  ray_version_major(void);
int  ray_version_minor(void);
int  ray_version_patch(void);
const char* ray_version_string(void);

/* ===== Type Constants ===== */

#define RAY_LIST       0
#define RAY_BOOL       1
#define RAY_U8         2
#define RAY_I16        3
#define RAY_I32        4
#define RAY_I64        5
#define RAY_F32        6
#define RAY_F64        7
#define RAY_DATE       8
#define RAY_TIME       9
#define RAY_TIMESTAMP 10
#define RAY_GUID      11
/* Unified dictionary-encoded string column (adaptive width) */
#define RAY_SYM       12
/* Variable-length string column (inline + pool) */
#define RAY_STR       13

/* Compound types */
#define RAY_INDEX     97   /* Accelerator index attached to a vector (see ops/idxop.h) */
#define RAY_TABLE     98
#define RAY_DICT      99

/* Function types (Rayforce-compatible) */
#define RAY_LAMBDA    100   /* User-defined function (compiled body + env) */
#define RAY_UNARY     101   /* Unary builtin: ray_t* (*)(ray_t*) */
#define RAY_BINARY    102   /* Binary builtin: ray_t* (*)(ray_t*, ray_t*) */
#define RAY_VARY      103   /* Variadic builtin: ray_t* (*)(ray_t**, int64_t) */
#define RAY_ERROR     127   /* Error object: 8-byte packed ASCII code in sdata */
#define RAY_NULL      126   /* Null / void — singleton static object */

/* ===== Error Handling ===== */

typedef enum {
    RAY_OK = 0,
    RAY_ERR_OOM,
    RAY_ERR_TYPE,
    RAY_ERR_RANGE,
    RAY_ERR_LENGTH,
    RAY_ERR_RANK,
    RAY_ERR_DOMAIN,
    RAY_ERR_NYI,
    RAY_ERR_IO,
    RAY_ERR_SCHEMA,
    RAY_ERR_CORRUPT,
    RAY_ERR_CANCEL,
    RAY_ERR_PARSE,
    RAY_ERR_NAME,
    RAY_ERR_LIMIT,
    RAY_ERR_RESERVED
} ray_err_t;

#define RAY_IS_ERR(p)    ((p) != NULL && (uintptr_t)(p) > 31 && ((ray_t*)(p))->type == RAY_ERROR)

/* ===== Core Type: ray_t (32-byte block/object header) ===== */

typedef union ray_t {
    /* Allocated: object header */
    struct {
        /* Bytes 0-15: nullable bitmask / slice / ext nullmap / index */
        union {
            uint8_t  nullmap[16];
            struct { union ray_t* slice_parent; int64_t slice_offset; };
            struct { union ray_t* ext_nullmap;  union ray_t* sym_dict; };
            struct { union ray_t* str_ext_null; union ray_t* str_pool; };
            /* RAY_ATTR_HAS_INDEX (vectors): ray_t* of type RAY_INDEX
             * carrying both the accelerator payload and the saved nullmap
             * bytes.  _idx_pad is reserved (must be NULL).  See ops/idxop.h. */
            struct { union ray_t* index;        union ray_t* _idx_pad; };
            /* RAY_ATTR_HAS_LINK (vectors, RAY_I32/RAY_I64 only): bytes 8-15
             * hold an int64 sym ID naming the target table.  link_lo[8]
             * aliases bytes 0-7 (inline nullmap bits OR ext_nullmap pointer
             * OR HAS_INDEX index pointer, depending on the other arm in use).
             * See ops/linkop.h. */
            struct { uint8_t link_lo[8];        int64_t link_target; };
        };
        /* Bytes 16-31: metadata + value */
        uint8_t  mmod;       /* 0=heap, 1=file-mmap */
        uint8_t  order;      /* block order (block size = 2^order) */
        int8_t   type;       /* negative=atom, positive=vector, 0=LIST */
        uint8_t  attrs;      /* attribute flags */
        uint32_t rc;         /* reference count (0=free) */
        union {
            uint8_t  b8;     /* BOOL atom */
            uint8_t  u8;     /* U8 atom */
            int16_t  i16;    /* I16 atom */
            int32_t  i32;    /* I32 atom */
            uint32_t u32;
            int64_t  i64;    /* I64/SYMBOL/DATE/TIME/TIMESTAMP atom */
            double   f64;    /* F64 atom */
            union ray_t* obj; /* pointer to child (long strings, GUID) */
            struct { uint8_t slen; char sdata[7]; }; /* SSO string (<=7 bytes) */
            int64_t  len;    /* vector element count */
        };
        uint8_t  data[];     /* element data (flexible array member) */
    };
    /* Free: buddy allocator block (fl_prev/fl_next overlay bytes 0-15) */
    struct {
        union ray_t* fl_prev;
        union ray_t* fl_next;
    };
} ray_t;

/* Global null singleton — always valid, retain/release are no-ops (ARENA flag) */
extern ray_t __ray_null;
#define RAY_NULL_OBJ  (&__ray_null)
#define RAY_IS_NULL(p) ((p) == RAY_NULL_OBJ)

/* Global last-resort OOM error sentinel — returned by ray_error when its
 * own ray_alloc fails (deep OOM, e.g. heap can't even satisfy the 32-byte
 * error header).  ARENA-flagged like RAY_NULL_OBJ so retain/release are
 * no-ops; slen=3 / sdata="oom" so RAY_IS_ERR() and ray_err_code() both
 * work without touching the heap.  Carries no per-VM message — we have
 * no heap to format one into.  Without this fallback, hard OOM would
 * silently bypass every `if (RAY_IS_ERR(x)) return x;` guard upstream. */
extern ray_t __ray_oom;
#define RAY_OOM_OBJ   (&__ray_oom)

/* Error object creation (defined in core/runtime.c) */
ray_t* ray_error(const char* code, const char* fmt, ...);
const char* ray_err_code_str(ray_err_t e);
ray_err_t ray_err_from_obj(ray_t* err);
const char* ray_err_code(ray_t* err);
/* Free a RAY_ERROR object.  ray_release() is a deliberate no-op for
 * error ray_t* (see src/mem/cow.c), so callers that hold the sole
 * reference and want the block reclaimed must use this helper instead —
 * otherwise the error leaks until heap teardown. */
void ray_error_free(ray_t* err);

/* ===== Accessor Macros ===== */

#define RAY_ATTR_SLICE  0x10

#define ray_type(v)       ((v)->type)
#define ray_is_atom(v)    ((v)->type < 0 || (v)->type >= RAY_LAMBDA)
#define ray_is_vec(v)     ((v)->type >= RAY_BOOL && (v)->type <= RAY_STR)
#define ray_len(v)        ((v)->len)

/* Element type sizes indexed by type tag — covers all uint8_t values.
 * Only types 1-14 (vectors) have non-zero entries. */
extern const uint8_t ray_type_sizes[256];

static inline void* ray_data_fn(ray_t* v) {
    if (__builtin_expect(!!(v->attrs & RAY_ATTR_SLICE), 0))
        return (char*)v->slice_parent->data
               + v->slice_offset * ray_type_sizes[(uint8_t)v->type];
    return (void*)v->data;
}
#define ray_slice_data(v) ray_data_fn(v)  /* alias — ray_data is always slice-safe */
#define ray_data(v)       ray_data_fn(v)

/* ===== Memory Allocator API ===== */

ray_t*    ray_alloc(size_t data_size);
/* NOTE: ray_free supports cross-thread free via foreign_blocks list.
 * Blocks freed from a non-owning thread are deferred and coalesced
 * when the owning heap flushes foreign blocks. */
void     ray_free(ray_t* v);

/* ===== Memory Budget API ===== */

int64_t  ray_mem_budget(void);      /* returns memory budget in bytes */
bool     ray_mem_pressure(void);    /* true if calling thread's usage exceeds budget */

/* ===== Interrupt API =====
 * Long-running queries poll ray_interrupted() at morsel granularity
 * and bail out with a "cancel" error. The REPL's SIGINT handler wires
 * Ctrl-C to ray_request_interrupt(); embedders can call it from their
 * own signal handlers or cancellation threads. */

void     ray_request_interrupt(void);
void     ray_clear_interrupt(void);
bool     ray_interrupted(void);

/* ===== Progress API =====
 * Pull-based, main-thread only. Worker threads never touch progress
 * state. The executor calls ray_progress_update() at natural sync
 * points (between ops, after pool dispatches, at pivot phase
 * boundaries); the update only fires the user callback once the
 * query has been running for at least min_ms and at most once per
 * tick_interval_ms. Embedders register a callback to visualize
 * long-running queries; leaving it unset has zero runtime cost. */

typedef struct {
    const char* op_name;      /* coarse: scan, group, pivot, join, ... */
    const char* phase;        /* optional finer label, e.g. "pivot: dedupe" */
    uint64_t    rows_done;
    uint64_t    rows_total;   /* 0 = indeterminate */
    double      elapsed_sec;
    int64_t     mem_used;     /* bytes: buddy + direct mmap */
    int64_t     mem_budget;   /* bytes: auto-detected memory budget */
    bool        final;        /* true on the last tick of a query — renderers
                                 use this to clear the line */
} ray_progress_t;

typedef void (*ray_progress_cb)(const ray_progress_t* snapshot, void* user);

/* Register a progress callback. Set cb=NULL to disable. min_ms is the
 * show-after threshold: queries finishing under it fire
 * zero callbacks. tick_interval_ms throttles updates once active. */
void ray_progress_set_callback(ray_progress_cb cb, void* user,
                                uint64_t min_ms, uint64_t tick_interval_ms);

/* Update progress state. Safe to call from the main thread only.
 * phase/op_name may be NULL to keep the previous value. Counters
 * always overwrite — 0 is a valid "starting fresh" value. Fires the
 * registered callback if the show-after and tick-interval gates pass. */
void ray_progress_update(const char* op_name, const char* phase,
                         uint64_t rows_done, uint64_t rows_total);

/* Relabel without touching the counters — for wrappers like exec_node
 * that only know which operator is about to run but not its rows. A
 * subsequent ray_progress_update from inside the op will advance the
 * counters; until then the renderer shows an indeterminate bar. */
void ray_progress_label(const char* op_name, const char* phase);

/* Mark the end of the current query. Clears state and fires a final
 * "100%" tick if the query ran long enough to have shown the bar. */
void ray_progress_end(void);

/* ===== COW / Ref Counting API ===== */

void     ray_retain(ray_t* v);
void     ray_release(ray_t* v);

/* ===== Atom Constructors ===== */

ray_t* ray_bool(bool val);
ray_t* ray_u8(uint8_t val);
ray_t* ray_i16(int16_t val);
ray_t* ray_i32(int32_t val);
ray_t* ray_i64(int64_t val);
ray_t* ray_f32(float val);
ray_t* ray_f64(double val);
ray_t* ray_str(const char* s, size_t len);
ray_t* ray_sym(int64_t id);
ray_t* ray_date(int64_t val);
ray_t* ray_time(int64_t val);
ray_t* ray_timestamp(int64_t val);
ray_t* ray_guid(const uint8_t* bytes);
ray_t* ray_typed_null(int8_t type);

/* Null bitmap check for atoms — bit 0 of nullmap[0] marks typed nulls.
 * Also matches RAY_NULL_OBJ (the untyped null singleton). */
#define RAY_ATOM_IS_NULL(x) (RAY_IS_NULL(x) || ((x)->type < 0 && ((x)->nullmap[0] & 1)))

/* ===== Vector API ===== */

ray_t* ray_vec_new(int8_t type, int64_t capacity);
ray_t* ray_sym_vec_new(uint8_t sym_width, int64_t capacity);  /* RAY_SYM with adaptive width */
ray_t* ray_vec_append(ray_t* vec, const void* elem);
ray_t* ray_vec_set(ray_t* vec, int64_t idx, const void* elem);
void* ray_vec_get(ray_t* vec, int64_t idx);
ray_t* ray_vec_slice(ray_t* vec, int64_t offset, int64_t len);
ray_t* ray_vec_concat(ray_t* a, ray_t* b);
ray_t* ray_vec_from_raw(int8_t type, const void* data, int64_t count);
ray_t* ray_vec_insert_at(ray_t* vec, int64_t idx, const void* elem);
ray_t* ray_vec_insert_vec_at(ray_t* vec, int64_t idx, ray_t* src);
ray_t* ray_vec_insert_many(ray_t* vec, ray_t* idxs, ray_t* vals);

/* Null bitmap ops */
void     ray_vec_set_null(ray_t* vec, int64_t idx, bool is_null);
ray_err_t ray_vec_set_null_checked(ray_t* vec, int64_t idx, bool is_null);
bool     ray_vec_is_null(ray_t* vec, int64_t idx);

/* ===== String Vector API ===== */

ray_t* ray_str_vec_append(ray_t* vec, const char* s, size_t len);
const char* ray_str_vec_get(ray_t* vec, int64_t idx, size_t* out_len);
ray_t* ray_str_vec_set(ray_t* vec, int64_t idx, const char* s, size_t len);
ray_t* ray_str_vec_insert_at(ray_t* vec, int64_t idx, const char* s, size_t len);
ray_t* ray_str_vec_compact(ray_t* vec);

/* ===== String API ===== */

const char* ray_str_ptr(ray_t* s);
size_t      ray_str_len(ray_t* s);
int         ray_str_cmp(ray_t* a, ray_t* b);

/* ===== List API ===== */

ray_t* ray_list_new(int64_t capacity);
ray_t* ray_list_append(ray_t* list, ray_t* item);
ray_t* ray_list_get(ray_t* list, int64_t idx);
ray_t* ray_list_set(ray_t* list, int64_t idx, ray_t* item);
ray_t* ray_list_insert_at(ray_t* list, int64_t idx, ray_t* item);
ray_t* ray_list_insert_many(ray_t* list, ray_t* idxs, ray_t* vals);

/* ===== Symbol Intern Table API ===== */

ray_err_t ray_sym_init(void);
void     ray_sym_destroy(void);
int64_t  ray_sym_intern(const char* str, size_t len);
int64_t  ray_sym_find(const char* str, size_t len);
ray_t*    ray_sym_str(int64_t id);
uint32_t ray_sym_count(void);
bool     ray_sym_ensure_cap(uint32_t needed);
ray_err_t ray_sym_save(const char* path);
ray_err_t ray_sym_load(const char* path);

/* ===== Environment API =====
 *
 * Thread-safety: the environment is shared global state.  Concurrent calls
 * to ray_env_get() and ray_env_set() require external synchronization by
 * the caller. */

ray_t*    ray_env_get(int64_t sym_id);
ray_err_t ray_env_set(int64_t sym_id, ray_t* val);

/* ===== Table API ===== */

ray_t*       ray_table_new(int64_t ncols);
ray_t*       ray_table_add_col(ray_t* tbl, int64_t name_id, ray_t* col_vec);
ray_t*       ray_table_get_col(ray_t* tbl, int64_t name_id);
ray_t*       ray_table_get_col_idx(ray_t* tbl, int64_t idx);
int64_t     ray_table_col_name(ray_t* tbl, int64_t idx);
void        ray_table_set_col_name(ray_t* tbl, int64_t idx, int64_t name_id);
int64_t     ray_table_ncols(ray_t* tbl);
int64_t     ray_table_nrows(ray_t* tbl);
ray_t*       ray_table_schema(ray_t* tbl);

/* ===== Dict API =====
 *
 * A dict is a 2-pointer block (type=RAY_DICT, len=2) holding [keys, vals].
 * Pair count is keys->len.
 *
 * keys:  Either a typed vector (RAY_SYM / RAY_I64 / RAY_F64 / RAY_STR /
 *        RAY_GUID / RAY_DATE / RAY_TIME / RAY_TIMESTAMP / RAY_I32 /
 *        RAY_I16 / RAY_BOOL / RAY_U8 / RAY_F32) when every key shares
 *        one atom type, or a RAY_LIST of boxed atoms when keys are
 *        heterogeneous.  Typed-vec lookup honors the keys' null bitmap
 *        so a null key never collides with a legitimate zero/sentinel.
 * vals:  Either a typed vector when every value shares one atom type,
 *        or a RAY_LIST otherwise (the form parsed from {…} literals,
 *        which keep value expressions unevaluated until probed).
 *
 * Layout matches RAY_TABLE; only the type tag and the contract on
 * `vals` (a RAY_LIST of column vectors for tables) differ.
 */

ray_t*  ray_dict_new(ray_t* keys, ray_t* vals);          /* consumes both */
ray_t*  ray_dict_keys(ray_t* d);                         /* borrowed */
ray_t*  ray_dict_vals(ray_t* d);                         /* borrowed */
int64_t ray_dict_len(ray_t* d);                          /* keys->len */
ray_t*  ray_dict_get(ray_t* d, ray_t* key_atom);         /* owned, NULL if missing */
ray_t*  ray_dict_upsert(ray_t* d, ray_t* key_atom, ray_t* val); /* COW; consumes d */
ray_t*  ray_dict_remove(ray_t* d, ray_t* key_atom);             /* COW; consumes d */

#ifdef __cplusplus
}
#endif

#endif /* RAY_H */
