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

/* The real version is injected at build time via -D from the git tag (the
 * single source of truth — see the Makefile and RELEASE.md). These are only
 * fallbacks: 0.0.0 marks an untagged/dev build, and they give downstream code
 * that includes this header without our -D flags a defined value. They are
 * #ifndef-guarded so the injected -D wins without a -Wmacro-redefined error
 * under -Werror. Do NOT hand-edit these to cut a release — tag instead. */
#ifndef RAY_VERSION_MAJOR
#define RAY_VERSION_MAJOR 0
#endif
#ifndef RAY_VERSION_MINOR
#define RAY_VERSION_MINOR 0
#endif
#ifndef RAY_VERSION_PATCH
#define RAY_VERSION_PATCH 0
#endif

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
    RAY_ERR_RESERVED,
    RAY_ERR_VERSION
} ray_err_t;

#define RAY_IS_ERR(p)    ((p) != NULL && (uintptr_t)(p) > 31 && ((ray_t*)(p))->type == RAY_ERROR)

/* ===== Core Type: ray_t (32-byte block/object header) ===== */

/* Symbol-resolution domain (src/table/domain.h): every RAY_SYM vector
 * carries a non-NULL pointer to one — either the immortal runtime
 * singleton (global intern table) or a refcounted mmapped symfile. */
struct ray_sym_domain_s;

typedef union ray_t {
    /* Allocated: object header */
    struct {
        /* Bytes 0-15: slice / str_pool / index / link arm.  Null state is
         * sentinel-encoded in the payload (see src/vec/vec.c); this 16-byte
         * slot carries no bitmap bits.  `aux` is the raw-byte view used by
         * atoms (aux[0]&1 typed-null bit), envs (builtin name @ aux[2..15]),
         * functions (DAG opcode @ aux[0..1]), str-pools (dead-bytes @
         * aux[0..3]), tables/dicts/lists (zero-init), and the on-disk col
         * header. */
        union {
            uint8_t  aux[16];
            struct { union ray_t* slice_parent;  int64_t slice_offset; };
            struct { uint8_t _aux_str_lo[8];     union ray_t* str_pool; };
            /* RAY_SYM (vectors): bytes 8-15 hold the symbol-domain
             * pointer (see src/table/domain.h) — every non-slice SYM vec
             * carries a non-NULL domain; runtime-created vecs point at
             * the immortal singleton wrapping the global intern table.
             * Layout audit (updated 2026-07): bytes 8-15 hold the domain;
             * bytes 0-7 are free for a SYM vec to carry an index —
             *   - str_pool uses 8-15 but only for RAY_STR;
             *   - HAS_INDEX CAN be set on a SYM vec (as of the SYM hash
             *     index): the index pointer occupies bytes 0-7 while
             *     sym_domain stays at bytes 8-15.  attach_finalize
             *     (ops/idxop.c) preserves _idx_pad (8-15) for RAY_SYM,
             *     exactly as it does for RAY_STR's str_pool.  That guard is
             *     the MANDATORY backstop — do NOT restore the _idx_pad
             *     clearing for SYM or it nulls the domain pointer;
             *   - HAS_LINK's link_target (8-15) is RAY_I32/RAY_I64 only;
             *   - slices use both 0-7 and 8-15, but slice headers do not
             *     store a domain — ray_sym_vec_domain follows
             *     slice_parent, mirroring ray_data_fn's slice walk.
             * The pointer is runtime-only state: serde strips aux, and
             * col_save zeroes the SYM header aux slot on disk. */
            struct { uint8_t _aux_sym_lo[8];     struct ray_sym_domain_s* sym_domain; };
            /* RAY_ATTR_HAS_INDEX (vectors): ray_t* of type RAY_INDEX
             * carrying the accelerator payload and the saved aux
             * bytes.  _idx_pad (8-15) is NULL for numeric columns, but
             * holds str_pool for RAY_STR and sym_domain for RAY_SYM
             * (attach_finalize preserves it there).  See ops/idxop.h. */
            struct { union ray_t* index;         union ray_t* _idx_pad; };
            /* RAY_ATTR_HAS_LINK (vectors, RAY_I32/RAY_I64 only): bytes
             * 8-15 hold an int64 sym ID naming the target table.
             * link_lo[8] aliases bytes 0-7.  See ops/linkop.h. */
            struct { uint8_t link_lo[8];         int64_t link_target; };
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
/* A value position (an eval'd / builtin-returned ray_t*) is never a bare C NULL —
 * value-null is always RAY_NULL_OBJ. Assert that invariant in debug/ASan builds;
 * compile to nothing in release. Placed BEFORE any null test that may deref,
 * because RAY_ATOM_IS_NULL/ray_atom_is_null_fn deref x->type on non-singleton input. */
/* Kept live in the hardened cloud tier too (not just DEBUG): the check is a
 * single pointer compare, and a stray C NULL in a value position is exactly
 * the class of corruption that must fail fast — routed through the crash
 * handler's SIGABRT backtrace — rather than segfault deep in a kernel. */
#if defined(DEBUG) || defined(RAY_HARDENED)
#define RAY_ASSERT_VALUE(x) assert((x) != NULL && "value-null must be RAY_NULL_OBJ, not C NULL")
#else
#define RAY_ASSERT_VALUE(x) ((void)0)
#endif

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

/* Out-of-line slice deref: keeps the hot path of ray_data_fn (a single
 * load + return) trivially inlinable while the rare slice arm lives in
 * one TU (vec.c) — avoids N inline instantiations of the slice branch
 * across every translation unit that includes this header. */
void* ray_data_slice_path(ray_t* v);

static inline void* ray_data_fn(ray_t* v) {
    if (__builtin_expect(!!(v->attrs & RAY_ATTR_SLICE), 0))
        return ray_data_slice_path(v);
    return (void*)v->data;
}
#define ray_slice_data(v) ray_data_fn(v)  /* alias — ray_data is always slice-safe */
#define ray_data(v)       ray_data_fn(v)

/* ===== Symbol domain access (RAY_SYM vectors) ===== */

/* The immortal runtime domain singleton wrapping the global intern table.
 * Full domain API lives in src/table/domain.h. */
struct ray_sym_domain_s* ray_sym_runtime_domain(void);

/* Resolution domain of a RAY_SYM vector.  Slices don't carry a domain of
 * their own — follow slice_parent, the same walk ray_data_fn performs.
 * Defensively returns the runtime singleton if the field is zero (e.g. a
 * producer that builds SYM headers by hand); uniform non-NULL guarantee
 * for consumers. */
static inline struct ray_sym_domain_s* ray_sym_vec_domain(ray_t* v) {
    if (v->attrs & RAY_ATTR_SLICE) v = v->slice_parent;
    struct ray_sym_domain_s* d = v->sym_domain;
    return d ? d : ray_sym_runtime_domain();
}

/* Domain resolution primitives (full API + docs: src/table/domain.h).
 * Declared here so the inline cell-resolution helpers next to
 * ray_read_sym (src/table/sym.h) compile without internal headers.
 *   str:  borrowed string atom for position `pos` (NULL if out of range);
 *   find: position of the bytes in the domain, -1 if absent. */
ray_t*  ray_sym_domain_str(struct ray_sym_domain_s* dom, int64_t pos);
int64_t ray_sym_domain_find(struct ray_sym_domain_s* dom, const char* str, size_t len);

/* Entry count, and the position → runtime-intern-id LUT (NULL for the
 * runtime domain — ids pass through; for FILE domains the FIRST request
 * interns the vocabulary sequentially: obtain the LUT BEFORE handing
 * cell ids to ray_pool_dispatch workers).  Docs: src/table/domain.h. */
int64_t ray_sym_domain_count(struct ray_sym_domain_s* dom);
const int64_t* ray_sym_domain_runtime_lut(struct ray_sym_domain_s* dom);

/* An output SYM vector built by COPYING cell ids from `in` (group keys,
 * filtered selections) must resolve over the same dictionary: release
 * out's previous non-singleton domain ref, copy the pointer, retain the
 * new non-singleton.  No-op unless both are RAY_SYM vectors (and never
 * touches slice headers — their aux bytes are parent/offset).  Defined
 * in src/vec/vec.c. */
void ray_sym_vec_adopt_domain(ray_t* out, ray_t* in);

/* ===== Introspection helpers (FFI-safe access for foreign consumers) ===== */

int8_t   ray_obj_type(ray_t* v);
uint8_t  ray_obj_attrs(ray_t* v);
int64_t  ray_vec_get_i64(ray_t* vec, int64_t idx);
double   ray_vec_get_f64(ray_t* vec, int64_t idx);
int64_t  ray_vec_get_sym_id(ray_t* vec, int64_t idx);

/* ===== Memory Allocator API ===== */

ray_t*    ray_alloc(size_t data_size);
/* NOTE: ray_free supports cross-thread free via foreign_blocks list.
 * Blocks freed from a non-owning thread are deferred and coalesced
 * when the owning heap flushes foreign blocks. */
void     ray_free(ray_t* v);

/* ===== Raw buffer allocator (buddy-backed, no libc malloc) =====
 * malloc/calloc/realloc/free replacements for plain byte/scalar buffers that
 * are NOT ray_t values — backed by the tuned slab/buddy heap (fast for the many
 * small per-partition allocations the agg engine makes), thread-safe, with
 * cross-thread free.  ray_alloc_raw returns uninitialised memory; ray_calloc_raw
 * zeroes it.  Free with ray_free_raw, grow with ray_realloc_raw. */
void*    ray_alloc_raw(size_t n);
void*    ray_calloc_raw(size_t n);
void*    ray_realloc_raw(void* p, size_t n);
void     ray_free_raw(void* p);

/* ===== System memory ===== */

int64_t  ray_sys_total_ram(void);   /* total physical RAM in bytes (informational) */

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
    int64_t     mem_used;     /* bytes: live object footprint (buddy + direct mmap) */
    int64_t     mem_total;    /* bytes: total physical RAM (the disk-spill threshold) */
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

/* True iff a progress callback is registered. The executor gates arming the
 * qstats PROGRESS bit on this, so all progress plumbing stays zero-cost when
 * nothing is listening. */
bool ray_progress_active(void);

/* Pool-dispatch pump (main-thread only). ray_progress_dispatch_begin records a
 * parallel dispatch's element total and baselines the shared qstats row
 * counter; ray_progress_pump samples it and fires the throttled callback. The
 * pool calls these from its claim-loop / spin-wait so every morsel-parallel op
 * reports progress with no per-kernel changes. Both are no-ops with no callback. */
void ray_progress_dispatch_begin(uint64_t phase_rows_total);
void ray_progress_pump(void);

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
/* Current UTC wall-clock time as a RAY_TIMESTAMP value (ns since 2000-01-01).
 * Shared "now as a timestamp" primitive — the single owner of the epoch
 * conversion; callers must not re-derive it. */
int64_t ray_timestamp_now_ns(void);
ray_t* ray_guid(const uint8_t* bytes);
ray_t* ray_typed_null(int8_t type);

/* ===== Null Sentinel Values =====
 *
 * Per-type null encoding for nullable scalar types.  Callers compare values
 * directly (e.g. `x == NULL_I64`, `x != x` for NaN); there are no predicate
 * macros or aliases.  Temporal types (DATE/TIME/TIMESTAMP) reuse NULL_I32 or
 * NULL_I64 based on their storage width.  SYM null = sym ID 0; STR null =
 * empty string (length 0); BOOL and U8 are non-nullable. */
#define NULL_I16  ((int16_t)INT16_MIN)
#define NULL_I32  ((int32_t)INT32_MIN)
#define NULL_I64  ((int64_t)INT64_MIN)
#define NULL_F32  ((float)__builtin_nanf(""))
#define NULL_F64  (__builtin_nan(""))

/* Atom null check.  RAY_NULL_OBJ is the untyped null singleton.
 * Typed atoms with a defined NULL_* sentinel use payload-compare;
 * types without a sentinel (BOOL/U8/F32) fall back to the
 * aux[0]&1 bit written by ray_typed_null. */
static inline bool ray_atom_is_null_fn(const union ray_t* x) {
    if (RAY_IS_NULL(x)) return true;
    if (x->type >= 0) return false;
    switch (-x->type) {
        case RAY_F64:       return x->f64 != x->f64;
        case RAY_F32: {
            /* F32 atoms reuse the f64 union slot — see ray_f32 / atom.c. */
            float f = (float)x->f64;
            return f != f;
        }
        case RAY_I64:
        case RAY_TIMESTAMP: return x->i64 == NULL_I64;
        case RAY_I32:
        case RAY_DATE:
        case RAY_TIME:      return x->i32 == NULL_I32;
        case RAY_I16:       return x->i16 == NULL_I16;
        case RAY_SYM:
            /* SYM has no null — sym 0 is the empty symbol ' (a value), mirroring
             * STR's empty "" below.  A SYM atom is never null. */
            return false;
        case RAY_STR:
            /* STR has no null distinct from "" (char lists have no null, only
             * symbols do).  A STR atom is never null — the empty
             * string is a value. */
            return false;
        case RAY_GUID: {
            /* GUID null = 16 all-zero bytes in obj's U8 buffer.
             * obj is always populated by ray_guid / ray_typed_null —
             * a NULL obj indicates corruption; treat as null
             * defensively. */
            if (!x->obj) return true;
            const uint8_t* b = (const uint8_t*)((char*)x->obj + sizeof(union ray_t));
            for (int i = 0; i < 16; i++) if (b[i]) return false;
            return true;
        }
        default:            return (x->aux[0] & 1) != 0;
    }
}
#define RAY_ATOM_IS_NULL(x) ray_atom_is_null_fn(x)

/* ===== Vector API ===== */

ray_t* ray_vec_new(int8_t type, int64_t capacity);

/* RAY_SYM index width — encoded in the lower 2 bits of the vector's
 * `attrs` byte.  Pick the smallest width that fits the destination
 * symbol-table size; W64 is the safe default when growing globally. */
#define RAY_SYM_W8    0  /* uint8_t  indices, ≤255 entries */
#define RAY_SYM_W16   1  /* uint16_t indices, ≤65,535 */
#define RAY_SYM_W32   2  /* uint32_t indices, ≤4,294,967,295 */
#define RAY_SYM_W64   3  /* int64_t  indices, unbounded */

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
ray_t* ray_str_vec_from_parts(const char* const* ptrs, const uint32_t* lens, const uint8_t* nulls, int64_t n);
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
int64_t  ray_sym_intern_runtime(const char* str, size_t len);
int64_t  ray_sym_find(const char* str, size_t len);
ray_t*    ray_sym_str(int64_t id);
uint32_t ray_sym_count(void);

/* Borrow a snapshot of the sym → string array.  Returns a pointer to
 * the underlying ray_t** strings table along with its length; valid
 * only while no concurrent ray_sym_intern occurs (i.e. read-only
 * execution phases).  Lock is taken once for the snapshot and dropped
 * before return — caller may iterate freely.  Both *out_strings and
 * *out_count must be non-NULL. */
void ray_sym_strings_borrow(ray_t*** out_strings, uint32_t* out_count);
bool     ray_sym_ensure_cap(uint32_t needed);

/* Runtime dictionary snapshot — NOT table symfiles.  ray_sym_save writes
 * the whole process-global intern table to `path` (tmp + fsync + atomic
 * rename, replacing whatever was there; single-writer contract);
 * ray_sym_load restores it, requiring every entry to land at the id equal
 * to its file position so ids from the saved session stay valid (see
 * ray_runtime_create_with_sym).  Stored tables do not use these: their
 * symbols live in per-table symfiles managed by the storage layer. */
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
void        ray_table_set_col_idx(ray_t* tbl, int64_t idx, ray_t* col_vec);
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

/* ===== Runtime + Rayfall Eval API =====
 *
 * Embedders build a runtime, optionally restoring a persisted symbol
 * table, then evaluate Rayfall source strings against the global env.
 *
 * Tables and other ray_t* values can be exposed to Rayfall by interning
 * a symbol name and binding via ray_env_set, then queried with
 * ray_eval_str:
 *
 *   ray_runtime_t* rt = ray_runtime_create_with_sym(".sym");
 *   int64_t name_id = ray_sym_intern("t", 1);
 *   ray_env_set(name_id, my_table);            // rayforce retains
 *   ray_t* result = ray_eval_str("select count from t");
 *   if (RAY_IS_ERR(result)) { ... } else { ...; ray_release(result); }
 *   ray_runtime_destroy(rt);
 *
 * Single-runtime: only one ray_runtime_t may be live at a time in the
 * same process. Creating a second without destroying the first is
 * undefined.  Symbols, env, and builtins are process-global; the
 * runtime owns their lifetime. */

typedef struct ray_runtime_s ray_runtime_t;

/* Create a runtime with an empty symbol table. argc/argv are forwarded
 * so embedders can hand off CLI flags; pass 0/NULL when not applicable. */
ray_runtime_t* ray_runtime_create(int argc, char** argv);

/* Create a runtime and pre-load the persisted symbol table at sym_path.
 * Missing file or NULL is fine (symbols start empty). I/O or corrupt-
 * file conditions are non-fatal: the runtime starts up and the caller
 * can detect the issue via ray_runtime_create_with_sym_err if needed. */
ray_runtime_t* ray_runtime_create_with_sym(const char* sym_path);

/* As ray_runtime_create_with_sym, but writes the sym-load status to
 * *out_sym_err. RAY_OK on success or absent file; non-zero on I/O,
 * corrupt-file, or OOM during sym load.  Pass NULL to ignore. */
ray_runtime_t* ray_runtime_create_with_sym_err(const char* sym_path,
                                                ray_err_t*  out_sym_err);

/* Tear down the runtime: lang state, env, VMs, heap.  Always pair with
 * exactly one ray_runtime_create* call. */
void ray_runtime_destroy(ray_runtime_t* rt);

/* Parse and evaluate a Rayfall source string against the global env.
 * Returns NULL for void / null results, an error ray_t* on failure
 * (test with RAY_IS_ERR and inspect with ray_err_code), or the result
 * value otherwise.  Caller owns the returned reference; release with
 * ray_release. */
ray_t* ray_eval_str(const char* source);

ray_t* ray_select(ray_t** args, int64_t n);
ray_t* ray_update(ray_t** args, int64_t n);
ray_t* ray_insert(ray_t** args, int64_t n);
ray_t* ray_upsert(ray_t** args, int64_t n);
ray_t* ray_fmt(ray_t* obj, int mode);

/* ===== IPC Client API =====
 *
 * Blocking client API for talking to a Rayforce IPC server. Handles are
 * process-local integer slots returned by ray_ipc_connect and released by
 * ray_ipc_close. ray_ipc_send performs a synchronous request/response
 * exchange; ray_ipc_send_async sends a fire-and-forget frame. */

int64_t   ray_ipc_connect(const char* host, uint16_t port,
                          const char* user, const char* password,
                          int timeout_ms);
void      ray_ipc_close(int64_t handle);
ray_t*    ray_ipc_send(int64_t handle, ray_t* msg);
ray_err_t ray_ipc_send_async(int64_t handle, ray_t* msg);
ray_t*    ray_ipc_send_verbose(int64_t handle, ray_t* msg);

/* ===== Append-Only Log (AOF) API =====
 *
 * Embeddable durable event log for host applications: opaque payloads,
 * monotonic contiguous LSNs, explicit commit barriers, segment rotation,
 * crash recovery.  Independent of the ray_runtime_t lifecycle — usable
 * before ray_runtime_create and after ray_runtime_destroy.
 *
 * Durability model: ray_aof_append() buffers and returns the reserved
 * LSN; ray_aof_commit() writes a commit frame into the stream and
 * fsyncs — every append issued before it is durable and becomes
 * observable once it returns RAY_OK.  Scans deliver only records that
 * precede the last commit frame, so uncommitted appends are invisible
 * BY CONSTRUCTION (not as an accident of buffering), no matter how
 * large the append or what reached the page cache.  A crash discards
 * only the un-committed suffix: on the next open, bytes after the last
 * commit frame are truncated and the LSN sequence continues from the
 * last committed record — LSNs of truncated records are reused, but
 * only ever for records no scan could have observed.  A CRC failure
 * BEFORE the last commit frame is damage to acknowledged data and
 * fails ray_aof_open with RAY_ERR_CORRUPT rather than truncating.
 * (The torn-tail tolerance is deliberate and differs from the
 * transaction journal, where a torn tail is fatal: AOF payloads are
 * opaque data the writer was never acknowledged for, not replayable
 * commands.)
 *
 * Threading: one writer handle per directory, no internal locking.
 * Concurrent ray_aof_scan() calls are safe, including against a live
 * writer — a scan observes the committed prefix and stops cleanly at
 * the first incomplete tail record.
 *
 * Errors: RAY_ERR_DOMAIN on bad arguments, RAY_ERR_IO on filesystem
 * failure, RAY_ERR_LIMIT on payload >= 2^32-1 bytes, RAY_ERR_CORRUPT
 * from open or scan when acknowledged (committed) data fails its CRC.
 */

typedef struct ray_aof_s ray_aof_t;

#define RAY_AOF_DEFAULT_SEGMENT_LIMIT ((int64_t)256 * 1024 * 1024)

/* Open (creating the directory if needed) and recover the log at `dir`.
 * `segment_limit` <= 0 selects RAY_AOF_DEFAULT_SEGMENT_LIMIT.  On
 * failure returns NULL and sets *out_err (out_err may be NULL). */
ray_aof_t* ray_aof_open(const char* dir, int64_t segment_limit,
                        ray_err_t* out_err);

/* Buffered append of `len` bytes; returns the reserved LSN (>= 0), or
 * -1 with *out_err set.  Durable only after ray_aof_commit(). */
int64_t    ray_aof_append(ray_aof_t* log, const void* payload,
                          int64_t len, ray_err_t* out_err);

/* Durability barrier: flush + fsync the active segment. */
ray_err_t  ray_aof_commit(ray_aof_t* log);

/* The LSN the next append will receive. */
int64_t    ray_aof_next_lsn(const ray_aof_t* log);

/* Scan callback: return true to continue, false to stop the scan. */
typedef bool (*ray_aof_scan_cb_t)(int64_t lsn, const void* payload,
                                  int64_t len, void* ctx);

/* Scan committed records with lsn >= from_lsn in LSN order, invoking
 * `cb` per record.  Needs no ray_aof_t handle — reads directly from
 * `dir`, concurrently with a live writer.  Returns the number of
 * records delivered, or -1 with *out_err set. */
int64_t    ray_aof_scan(const char* dir, int64_t from_lsn,
                        ray_aof_scan_cb_t cb, void* ctx,
                        ray_err_t* out_err);

/* Flush, fsync and close; frees the handle.  Returns the commit error
 * if the final flush fails (the handle is freed either way). */
ray_err_t  ray_aof_close(ray_aof_t* log);

#ifdef __cplusplus
}
#endif

#endif /* RAY_H */
