# Collections

Vectors, lists, tables, dictionaries, and selection bitmaps — the compound data structures that organize scalar values into queryable datasets.

## Vectors

Vectors are the fundamental columnar data structure in Rayforce. A vector is a typed, contiguous array of scalar elements — every element shares the same type. Vectors are the columns inside tables and the operands in every DAG operation.

```lisp
; I64 vector (integer literals)
ray> [1 2 3 4 5]
[1 2 3 4 5]

; F64 vector (float literals)
ray> [1.0 2.5 3.14]
[1.0 2.5 3.14]

; SYM vector (unquoted identifiers)
ray> [AAPL GOOG MSFT]
[AAPL GOOG MSFT]

; BOOL vector
ray> [true false true]
[true false true]
```

### Morsel Iteration

All vector processing in Rayforce happens in **morsels** — fixed-size chunks of 1024 elements. The executor never processes an entire column at once. Instead, it iterates morsel by morsel, which keeps data in L1/L2 cache and enables pipeline parallelism.

```c
// C API: morsel iteration
ray_morsel_t m;
ray_morsel_init(&m, vec);

while (ray_morsel_next(&m)) {
    // m.base   — pointer to element data for this morsel
    // m.count  — number of elements (up to 1024)
    // m.selection — RAY_SEL bitmap (NULL = all pass)
    process_morsel(&m);
}
```

!!! note "Why 1024?"
    A morsel of 1024 int64 elements is 8 KB — fits comfortably in L1 cache on modern CPUs. This size was chosen to balance cache utilization against morsel scheduling overhead.

### Null Handling

Null state is encoded in-band via a type-correct sentinel in the payload:
`INT64_MIN` for `i64`, `NaN` for `f64`, `INT32_MIN` for `i32`/`date`/`time`,
and so on. `RAY_ATTR_HAS_NULLS` is a fast "may contain nulls" hint; when it is
clear, null-aware code paths are skipped entirely.

```c
// C API: null state
ray_vec_set_null(vec, 3, true);   // mark index 3 as null
ray_vec_is_null(vec, 3);           // returns true
ray_vec_is_null(vec, 0);           // returns false
```

### COW Semantics

Vectors use copy-on-write (COW) reference counting. Multiple consumers can share the same vector via `ray_retain()`. Mutation goes through `ray_cow()`, which returns the same pointer if the reference count is 1, or a fresh copy if shared.

```c
// C API: COW pattern
ray_retain(vec);           // rc: 1 → 2 (shared)

ray_t* writable = ray_cow(vec);
if (writable != vec) {
    // Got a fresh copy — vec is still shared
    // Must release writable on error paths
}
// Safe to mutate writable
```

### Vector Operations

| C Function | Description |
|---|---|
| `ray_vec_new(type, cap)` | Allocate an empty vector with capacity |
| `ray_vec_append(vec, elem)` | Append one element (may reallocate) |
| `ray_vec_set(vec, idx, elem)` | Set element at index |
| `ray_vec_get(vec, idx)` | Get pointer to element at index |
| `ray_vec_slice(vec, off, len)` | Zero-copy slice (shares data) |
| `ray_vec_concat(a, b)` | Concatenate two vectors |
| `ray_vec_from_raw(type, data, n)` | Create from existing data array |
| `ray_str_vec_append(vec, s, len)` | Append a string to RAY_STR vector |
| `ray_str_vec_get(vec, idx, &len)` | Get string at index |

## Lists

Lists are boxed, heterogeneous containers. Each element is a `ray_t*` pointer to any Rayforce object. Lists are the backbone of table column storage: a table's columns are held in a list.

```lisp
; Create a list of mixed vectors
ray> (list [1 2 3] [A B C])
; => ([1 2 3] [A B C])

; Lists can hold any type
ray> (list 42 "hello" [1 2])
; => (42 "hello" [1 2])
```

```c
// C API: list operations
ray_t* lst = ray_list_new(4);          // initial capacity 4
lst = ray_list_append(lst, vec1);      // append element
lst = ray_list_append(lst, vec2);
ray_t* item = ray_list_get(lst, 0);    // get by index
lst = ray_list_set(lst, 0, new_item);  // replace at index
```

Lists have `type = RAY_LIST` (0) and store pointers in their `data[]` array. The `len` field tracks the number of elements.

## Tables

A table is a collection of named column vectors, all the same length. Tables are the primary data structure for analytical queries — the target of `select`, `update`, joins, and aggregations.

```lisp
; Create a table with column names and data
ray> (set t (table [sym price qty] (list [AAPL GOOG MSFT] [150.0 140.0 380.0] [100 200 50])))

; Query with select
ray> (select {from:t where: (> price 145.0)})
```

### Internal Structure

A table is a `ray_t` with `type = RAY_TABLE` (98). Internally it contains:

- **Schema** — an I64 vector of symbol IDs, one per column name
- **Columns** — a list of typed vectors, one per column

```c
// C API: table construction
ray_t* tbl = ray_table_new(3);               // 3 columns
ray_table_add_col(tbl, sym_id, col_vec);      // add named column

// Access
ray_t* col = ray_table_get_col(tbl, sym_id); // by name (symbol ID)
ray_t* col = ray_table_get_col_idx(tbl, 0);  // by position
int64_t nr = ray_table_nrows(tbl);           // row count
int64_t nc = ray_table_ncols(tbl);           // column count
ray_t* sch = ray_table_schema(tbl);          // I64 vec of col name IDs
```

### Column Name Management

| C Function | Description |
|---|---|
| `ray_table_col_name(tbl, idx)` | Get symbol ID of column at index |
| `ray_table_set_col_name(tbl, idx, id)` | Rename column at index |
| `ray_table_schema(tbl)` | Get the full schema as an I64 vector |

## Dictionaries

Dictionaries share the same physical layout as tables — a 2-pointer block (`type = RAY_DICT`, `len = 2`) holding a *keys* container and a *vals* container.  Pair count is `keys->len`; the helpers `ray_dict_keys` / `ray_dict_vals` / `ray_dict_len` / `ray_dict_get` / `ray_dict_upsert` / `ray_dict_remove` wrap the layout.

Supported *keys* shapes:

- A typed vector — any of `RAY_SYM`, `RAY_BOOL`, `RAY_U8`, `RAY_I16`, `RAY_I32`, `RAY_I64`, `RAY_F32`, `RAY_F64`, `RAY_DATE`, `RAY_TIME`, `RAY_TIMESTAMP`, `RAY_STR`, `RAY_GUID`.  Lookup is value-equality; sentinel nulls are honored so `0Nl` doesn't collide with a real `0`.
- A `RAY_LIST` of boxed atoms — used for heterogeneous keys (the only shape that can hold both `'sym` and `"str"` keys in the same dict).  Lookup falls back to `atom_eq`.

Supported *vals* shapes:

- A typed vector when every value shares one atom type (the form `group` emits — its index columns happen to all be `RAY_I64` vectors held inside a `RAY_LIST`; see below).
- A `RAY_LIST` when values are heterogeneous, are themselves containers (vectors / dicts / tables / functions), or need to stay unevaluated.  Parsed dictionary literals hold value syntax in this shape. Ordinary evaluation constructs a fresh dictionary by evaluating each value; querying a dictionary with `at` does not evaluate its stored contents.

In an ordinary expression, literal values evaluate left to right, once, in
the enclosing scope. `{a: x b: (+ x 1)}` reads `x` and computes the second
value. Nested literals evaluate recursively; keys remain data. Write a
literal symbol value quoted, such as `{column: 'CounterID}`. Bare unresolved
names raise a name error. Empty literals remain empty dictionaries.

`quote` preserves the literal as syntax: `(quote {a: x})` stores the name,
and `(eval (quote {a: x}))` constructs the dictionary. Dictionaries built as
runtime data by `dict` or the C API remain self-evaluating, so `eval` does not
execute arbitrary lists stored in them. Completed literal results are also
runtime data. Repeated evaluation of the syntax constructs fresh values.

Query special forms receive their arguments unevaluated: `select`, `update`
and `window` still interpret their literal dictionaries in query context.
When building an expression payload with an ordinary `list` call, quote a
static query dictionary to preserve it for that later query evaluation.
See [IPC expression payloads](../storage/ipc.md#expression-payloads).

The `{…}` literal is the convenient surface but is narrower than the API.  The parser only emits two key shapes:

- Bareword keys → `RAY_SYM` vec.  Identifier characters (letters, digits, `_`, `-`) are *all* interned as symbols — so `{1: "a"}` looks like an integer-keyed dict but actually stores the symbol named `"1"`, and `(at d 1)` with an `i64` atom will *miss*.
- Quoted-string keys → `RAY_STR` vec.  A mix of bareword and quoted keys in the same literal falls back to a `RAY_LIST` of boxed atoms.

To build a dict whose keys are real `RAY_I64` / `RAY_GUID` / temporal values — i.e. ones a numeric or temporal lookup atom will actually match — use `(dict keys vals)` with an explicitly typed keys vector or call `ray_dict_new` from C.

```lisp
; Dictionary literal (curly braces)
ray> {name: "Alice" age: 30 active: true}
{name: "Alice" age: 30 active: true}

; Access by key
ray> (set d {x: 10 y: 20})
ray> (get d 'x)
10
```

Dictionaries are used extensively in Rayfall for passing named arguments to query forms like `select` and `update`:

```lisp
; The select argument is a dictionary
ray> (set t2 (table [x y] (list [1 2 3] [A B C])))
ray> (select {from:t2 where: (> x 1) x:x x2: (* x x)})
```

## Selection Bitmaps (RAY_SEL)

A selection bitmap is a lazy filter representation used internally by the query optimizer and executor. Instead of materializing filtered rows into a new vector, Rayforce tracks which rows pass the filter as a compact bitmap.

### Segment Flags

Selections are organized in segments matching the morsel size (1024 elements). Each segment carries a flag that enables fast short-circuiting:

| Flag | Constant | Meaning |
|---|---|---|
| **NONE** | `RAY_SEL_NONE` (0) | All bits zero — skip entire morsel, no rows pass |
| **ALL** | `RAY_SEL_ALL` (1) | All bits one — process without checking bitmap |
| **MIX** | `RAY_SEL_MIX` (2) | Mixed bits — must check bitmap per row |

!!! note "Why lazy?"
    Selection bitmaps avoid materializing intermediate results during predicate evaluation. The optimizer can push selections backward through `OP_EXPAND` chains (sideways information passing) and compose multiple predicates by ANDing bitmaps — all without copying data.

### Block Layout

A `RAY_SEL` object has `type = 14` and a variable-size layout in its `data[]` region:

- **Segment flags** — one `uint8_t` per morsel (NONE/ALL/MIX)
- **Segment popcounts** — one `int32_t` per morsel (number of set bits)
- **Bit arrays** — 16 `uint64_t` words per morsel (1024 bits)

```c
// C API: bitmap manipulation
RAY_SEL_BIT_TEST(bits, row);   // test if row passes
RAY_SEL_BIT_SET(bits, row);    // mark row as passing
RAY_SEL_BIT_CLR(bits, row);    // mark row as filtered

// Convert a BOOL vector to a selection bitmap
ray_t* sel = ray_sel_from_pred(bool_vec);
```

## Partitioned Columns

Partitioned tables split data across multiple segments (typically by date). Each column in a partitioned table uses a **parted type** that wraps multiple memory-mapped vector segments into a single logical column.

### Type Encoding

Parted types are encoded as `RAY_PARTED_BASE + base_type`. For example, a partitioned I64 column has type `32 + 5 = 37`. The base type is recovered with `RAY_PARTED_BASETYPE(t)`.

| Constant | Value | Description |
|---|---|---|
| `RAY_PARTED_BASE` | 32 | Base offset for parted types |
| `RAY_MAPCOMMON` | 64 | Virtual partition column (e.g., date) |

### MAPCOMMON

When loading a date-partitioned table, Rayforce creates a virtual `RAY_MAPCOMMON` column. This column does not store actual data — it derives values from the partition directory names (e.g., `2024.01.15/`). Each row in a partition shares the same date value, so the MAPCOMMON column can represent millions of rows with zero per-row storage.

```c
// C API: load a date-partitioned table
ray_t* trades = ray_part_load("db", "trades");

// The 'date' column is MAPCOMMON — derived from directory names
// Queries that filter on date trigger partition pruning
```

!!! note "Partition pruning"
    The optimizer recognizes filters on MAPCOMMON columns and eliminates entire partitions from the scan — skipping their memory-mapped segments entirely. A query filtering on a single date in a year of data reads only 1/365th of the files.
