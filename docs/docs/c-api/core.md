# Core C API Reference

!!! note "Headers"
    Include `<rayforce.h>` for the bulk of the public API. A few internal helpers documented here live in their own headers — in particular `ray_cow` and the `ray_heap_init` / `ray_heap_destroy` lifecycle calls are declared in `"mem/heap.h"`, not in `<rayforce.h>`. Link against `librayforce.a` (static) or `librayforce.so` (shared). No external dependencies.

## Lifecycle

Every program must initialize the heap allocator and symbol table before using any other API. Destroy them in reverse order when done.

### ray_heap_init

Initializes the thread-local buddy allocator, slab cache, and memory subsystem. Must be called once per thread before any allocation. The main thread must call this before any other Rayforce function.

```c
void ray_heap_init(void);
```

### ray_heap_destroy

Tears down the thread-local heap, releasing all buddy blocks and slab caches back to the OS. Call once per thread at shutdown, after releasing all `ray_t` objects.

```c
void ray_heap_destroy(void);
```

### ray_sym_init

Initializes the global symbol intern table. Must be called after `ray_heap_init()` and before interning any symbols, loading CSV files, or creating tables with symbol columns. Returns `RAY_OK` on success.

```c
ray_err_t ray_sym_init(void);
```

### ray_sym_destroy

Frees the global symbol intern table and its arena-backed storage. Call at shutdown, before `ray_heap_destroy()`.

```c
void ray_sym_destroy(void);
```

#### Typical lifecycle pattern

```c
#include <rayforce.h>

int main(int argc, char** argv) {
    ray_runtime_t* runtime = ray_runtime_create(argc, argv);
    if (!runtime) return 1;

    /* ... use Rayforce API ... */

    ray_runtime_destroy(runtime);
    return 0;
}
```

## Memory Management

Rayforce uses its own buddy allocator with slab cache. Never use `malloc`/`free` for `ray_t` objects. All objects use COW (copy-on-write) ref counting for safe sharing.

### ray_alloc

Allocates a `ray_t` block with at least `data_size` bytes of data space (beyond the 32-byte header). Uses the slab cache for common sizes, buddy allocator for larger blocks. The returned block has `rc = 1`. Returns `NULL` on OOM.

```c
ray_t* ray_alloc(size_t data_size);
```

### ray_free

Returns a `ray_t` block to the allocator. Supports cross-thread free: blocks freed from a non-owning thread are enqueued to a lock-free LIFO and reclaimed when the owning heap flushes foreign blocks. Do not call directly on shared objects — use `ray_release()` instead.

```c
void ray_free(ray_t* v);
```

### ray_retain

Increments the reference count of a `ray_t` object. Call when storing an additional reference to an object (e.g., adding a column to a table while keeping the original pointer).

```c
void ray_retain(ray_t* v);
```

### ray_release

Decrements the reference count. When `rc` reaches zero, the object and any owned children are freed. This is the primary way to dispose of `ray_t` objects. Safe to call on `NULL`.

```c
void ray_release(ray_t* v);
```

### ray_cow

Copy-on-write: if `v` is the sole owner (`rc == 1`), returns `v` unchanged. Otherwise, creates a deep copy and returns it with `rc = 1`. The original is not modified. After `ray_cow()`, if the returned pointer differs from the original, the caller must release it on error paths.

```c
ray_t* ray_cow(ray_t* v);
```

#### COW pattern

```c
ray_t* original = ray_vec_new(RAY_I64, 100);
ray_retain(original);  /* rc = 2: shared */

ray_t* writable = ray_cow(original);
/* writable != original (copy was made because rc > 1) */
/* Now safe to mutate writable without affecting original */

/* On error paths, release the copy: */
if (writable != original) ray_release(writable);
```

## Atom Constructors

Atoms are scalar values stored in the `ray_t` header (no trailing data). Each constructor returns a heap-allocated `ray_t*` with `rc = 1` and a negative `type` field indicating an atom.

### ray_bool

Creates a boolean atom. The value is stored in the `b8` field of the header.

```c
ray_t* ray_bool(bool val);
```

### ray_u8

Creates an unsigned 8-bit integer atom.

```c
ray_t* ray_u8(uint8_t val);
```

### ray_i16

Creates a signed 16-bit integer atom.

```c
ray_t* ray_i16(int16_t val);
```

### ray_i32

Creates a signed 32-bit integer atom.

```c
ray_t* ray_i32(int32_t val);
```

### ray_i64

Creates a signed 64-bit integer atom. Also used for symbol IDs, dates, times, and timestamps internally.

```c
ray_t* ray_i64(int64_t val);
```

### ray_f64

Creates a 64-bit floating-point atom.

```c
ray_t* ray_f64(double val);
```

### ray_str

Creates a string atom. Strings of 7 bytes or fewer are stored inline (SSO in the header). Longer strings allocate a child block. The string is copied; the caller retains ownership of `s`.

```c
ray_t* ray_str(const char* s, size_t len);
```

### ray_sym

Creates a symbol atom from an intern table ID. Use `ray_sym_intern()` to get the ID first. The symbol references the global intern table; the atom itself stores only the integer ID.

```c
ray_t* ray_sym(int64_t id);

/* Usage: */
int64_t id = ray_sym_intern("hello", 5);
ray_t* s = ray_sym(id);
```

### ray_date

Creates a date atom. The value is an integer representing days since epoch (2000-01-01).

```c
ray_t* ray_date(int64_t val);
```

### ray_time

Creates a time atom. The value is milliseconds since midnight.

```c
ray_t* ray_time(int64_t val);
```

### ray_timestamp

Creates a timestamp atom. The value is nanoseconds since epoch.

```c
ray_t* ray_timestamp(int64_t val);
```

### ray_guid

Creates a 128-bit GUID atom from a 16-byte array.

```c
ray_t* ray_guid(const uint8_t* bytes);
```

## Vector API

Vectors are the fundamental columnar container. Each vector has a typed element array following the 32-byte header. All vector operations process data in 1024-element morsels.

### ray_vec_new

Creates an empty vector with the given element type and initial capacity. The vector length is 0; capacity determines pre-allocated space to avoid reallocation during appends.

```c
ray_t* ray_vec_new(int8_t type, int64_t capacity);

/* Create a 64-bit integer vector with room for 1000 elements */
ray_t* v = ray_vec_new(RAY_I64, 1000);
```

### ray_vec_append

Appends a single element to the end of the vector. The `elem` pointer must point to a value of the correct type and size. May reallocate if capacity is exceeded, returning a new pointer. The old pointer may be invalidated.

```c
ray_t* ray_vec_append(ray_t* vec, const void* elem);

int64_t val = 42;
v = ray_vec_append(v, &val);  /* always reassign! */
```

### ray_vec_set

Sets the element at index `idx`. The index must be in range `[0, len)`. Uses COW internally: if the vector is shared (`rc > 1`), a copy is made first. Returns the (possibly new) vector pointer.

```c
ray_t* ray_vec_set(ray_t* vec, int64_t idx, const void* elem);
```

### ray_vec_get

Returns a pointer to the element at index `idx`. The returned pointer is valid until the vector is reallocated or freed. Returns `NULL` if `idx` is out of range.

```c
void* ray_vec_get(ray_t* vec, int64_t idx);

int64_t* p = (int64_t*)ray_vec_get(v, 0);
if (p) printf("%lld\n", *p);
```

### ray_vec_slice

Creates a zero-copy slice referencing the original vector's data from `offset` to `offset + len`. The slice retains the parent (incrementing its `rc`), so the parent must not be freed while the slice exists. Mutations require COW.

```c
ray_t* ray_vec_slice(ray_t* vec, int64_t offset, int64_t len);

/* Zero-copy view of elements [10..20) */
ray_t* s = ray_vec_slice(v, 10, 10);
```

### ray_vec_concat

Creates a new vector containing all elements of `a` followed by all elements of `b`. Both vectors must have the same type. Returns a new vector with `rc = 1`.

```c
ray_t* ray_vec_concat(ray_t* a, ray_t* b);
```

### ray_vec_from_raw

Creates a vector by copying `count` elements from a raw C array. The data is copied; the caller retains ownership of the source array.

```c
ray_t* ray_vec_from_raw(int8_t type, const void* data, int64_t count);

int64_t vals[] = {10, 20, 30};
ray_t* v = ray_vec_from_raw(RAY_I64, vals, 3);
```

#### Null bitmap operations

```c
/* Set element at index 5 as null */
ray_vec_set_null(vec, 5, true);

/* Check if element is null */
if (ray_vec_is_null(vec, 5)) { /* ... */ }

/* Checked version returns RAY_ERR_RANGE on out-of-bounds */
ray_err_t err = ray_vec_set_null_checked(vec, idx, true);
```

## String Vector API

RAY_STR vectors have a specialized API because elements are variable-length. These functions handle the SSO/pool layout transparently.

### ray_str_vec_append

Appends a string to a RAY_STR vector. Strings of 12 bytes or fewer are stored inline; longer strings are written to the per-vector pool. May reallocate the vector and/or pool. Always reassign the return value.

```c
ray_t* ray_str_vec_append(ray_t* vec, const char* s, size_t len);

ray_t* v = ray_vec_new(RAY_STR, 100);
v = ray_str_vec_append(v, "hello", 5);
v = ray_str_vec_append(v, "a longer string here", 20);
```

### ray_str_vec_get

Returns a pointer to the string data at index `idx` and writes the length to `*out_len`. The returned pointer is valid until the vector is modified. Returns `NULL` if the element is null or index is out of range. The string is not null-terminated.

```c
const char* ray_str_vec_get(ray_t* vec, int64_t idx, size_t* out_len);

size_t len;
const char* s = ray_str_vec_get(v, 0, &len);
if (s) printf("%.*s\n", (int)len, s);
```

### ray_str_vec_set

Replaces the string at index `idx`. Uses COW if the vector is shared. The old pool data is not reclaimed immediately; use `ray_str_vec_compact()` to reclaim unused pool space.

```c
ray_t* ray_str_vec_set(ray_t* vec, int64_t idx, const char* s, size_t len);
```

### ray_str_vec_compact

Rebuilds the string pool, removing unreferenced data and compacting live strings. Use after many `ray_str_vec_set()` calls that leave dead pool entries.

```c
ray_t* ray_str_vec_compact(ray_t* vec);
```

## List API

Lists are heterogeneous containers holding `ray_t*` pointers. Used for table column lists, function argument lists, and mixed-type collections.

### ray_list_new

Creates an empty list with initial capacity for `capacity` items.

```c
ray_t* ray_list_new(int64_t capacity);
```

### ray_list_append

Appends an item to the list. The item is retained (rc incremented). May reallocate, so always reassign the return value.

```c
ray_t* ray_list_append(ray_t* list, ray_t* item);
```

### ray_list_get

Returns the item at index `idx`. Does not increment the reference count. Returns `NULL` if out of range.

```c
ray_t* ray_list_get(ray_t* list, int64_t idx);
```

### ray_list_set

Replaces the item at index `idx`. The old item is released and the new item is retained.

```c
ray_t* ray_list_set(ray_t* list, int64_t idx, ray_t* item);
```

## Table API

Tables are the primary data structure: a list of named columns (vectors) with a shared row count. Column names are symbol IDs from the global intern table.

### ray_table_new

Creates an empty table with capacity for `ncols` columns. Columns are added with `ray_table_add_col()`.

```c
ray_t* ray_table_new(int64_t ncols);
```

### ray_table_add_col

Adds a named column to the table. The `name_id` is a symbol ID (from `ray_sym_intern()`). The column vector is retained. Returns the table pointer (may reallocate).

```c
ray_t* ray_table_add_col(ray_t* tbl, int64_t name_id, ray_t* col_vec);

int64_t name = ray_sym_intern("price", 5);
tbl = ray_table_add_col(tbl, name, price_vec);
```

### ray_table_get_col

Looks up a column by symbol ID. Returns the column vector or `NULL` if not found. Does not increment the reference count.

```c
ray_t* ray_table_get_col(ray_t* tbl, int64_t name_id);
```

### ray_table_ncols

Returns the number of columns in the table.

```c
int64_t ray_table_ncols(ray_t* tbl);
```

### ray_table_nrows

Returns the number of rows (length of the first column). Returns 0 if the table has no columns.

```c
int64_t ray_table_nrows(ray_t* tbl);
```

#### Complete table example

```c
ray_heap_init();
ray_sym_init();

/* Create a 3-column table */
ray_t* ids    = ray_vec_from_raw(RAY_I64, (int64_t[]){1, 2, 3}, 3);
ray_t* prices = ray_vec_from_raw(RAY_F64, (double[]){9.99, 19.99, 29.99}, 3);

ray_t* names = ray_vec_new(RAY_STR, 3);
names = ray_str_vec_append(names, "Widget", 6);
names = ray_str_vec_append(names, "Gadget", 6);
names = ray_str_vec_append(names, "Doohickey", 9);

ray_t* tbl = ray_table_new(3);
tbl = ray_table_add_col(tbl, ray_sym_intern("id", 2), ids);
tbl = ray_table_add_col(tbl, ray_sym_intern("price", 5), prices);
tbl = ray_table_add_col(tbl, ray_sym_intern("name", 4), names);

printf("cols: %lld, rows: %lld\n",
       ray_table_ncols(tbl), ray_table_nrows(tbl));
/* Output: cols: 3, rows: 3 */

ray_release(ids);
ray_release(prices);
ray_release(names);
ray_release(tbl);

ray_sym_destroy();
ray_heap_destroy();
```

## Error Handling

Rayforce uses two error mechanisms: `ray_err_t` enum return codes for non-pointer functions, and error objects (`RAY_ERROR`) for pointer-returning functions.

### ray_error

Creates an error object with an 8-byte packed ASCII code and a formatted message. The error object is a `ray_t` with `type == RAY_ERROR`. Use `RAY_IS_ERR()` to test return values.

```c
ray_t* ray_error(const char* code, const char* fmt, ...);

/* Create a type error */
return ray_error("type", "expected I64, got F64");
```

### RAY_IS_ERR(p)

Macro. Tests whether a `ray_t*` is an error object. Safe to call on `NULL`. Returns `true` if `p` is non-null and has `type == RAY_ERROR`.

```c
ray_t* result = ray_execute(g, root);
if (RAY_IS_ERR(result)) {
    printf("Error: %s\n", ray_err_code(result));
    ray_error_free(result);
    return 1;
}
```

#### Error codes

| Code | Enum | Description |
|---|---|---|
| `RAY_OK` | 0 | Success |
| `RAY_ERR_OOM` | 1 | Out of memory |
| `RAY_ERR_TYPE` | 2 | Type mismatch |
| `RAY_ERR_RANGE` | 3 | Index out of range |
| `RAY_ERR_LENGTH` | 4 | Length mismatch |
| `RAY_ERR_RANK` | 5 | Rank error |
| `RAY_ERR_DOMAIN` | 6 | Domain error (invalid value) |
| `RAY_ERR_NYI` | 7 | Not yet implemented |
| `RAY_ERR_IO` | 8 | I/O error |
| `RAY_ERR_SCHEMA` | 9 | Schema mismatch |
| `RAY_ERR_CORRUPT` | 10 | Data corruption |
| `RAY_ERR_CANCEL` | 11 | Operation cancelled |
| `RAY_ERR_PARSE` | 12 | Parse error |
| `RAY_ERR_NAME` | 13 | Name not found |
| `RAY_ERR_LIMIT` | 14 | Limit exceeded |
| `RAY_ERR_RESERVED` | 15 | Reserved name |
| `RAY_ERR_VERSION` | 16 | Version mismatch |
