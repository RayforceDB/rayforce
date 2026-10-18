# Scalar Data Types

Every value in Rayforce has a type. Scalars are the atomic building blocks — integers, floats, dates, strings, and more. Vectors are typed arrays of these scalars.

## Type Reference

All scalar types, their internal constants, storage sizes, and Rayfall literal syntax:

| Name | Constant | Value | Size | Null Literal | Rayfall Literal |
|---|---|---|---|---|---|
| **Boolean** | `RAY_BOOL` | 1 | 1 byte | none | `true` / `false` |
| **Unsigned byte** | `RAY_U8` | 2 | 1 byte | none | `0x42` |
| **Short integer** | `RAY_I16` | 3 | 2 bytes | `0Nh` | Small integers |
| **Integer** | `RAY_I32` | 4 | 4 bytes | `0Ni` | `42i` |
| **Long integer** | `RAY_I64` | 5 | 8 bytes | `0Nl` | `42` |
| **Single float** | `RAY_F32` | 6 | 4 bytes | sentinel | — |
| **Double float** | `RAY_F64` | 7 | 8 bytes | `0Nf` | `3.14` |
| **Date** | `RAY_DATE` | 8 | 4 bytes | `0Nd` | `2024.01.15` |
| **Time** | `RAY_TIME` | 9 | 4 bytes | `0Nt` | `09:30:00.000` |
| **Timestamp** | `RAY_TIMESTAMP` | 10 | 8 bytes | `0Np` | `2024.01.15D09:30:00.000000000` |
| **GUID** | `RAY_GUID` | 11 | 16 bytes | all-zero GUID | UUID format |
| **Symbol** | `RAY_SYM` | 12 | Adaptive (8/16/32/64-bit) | `'` (empty symbol) | `'AAPL`, `[AAPL GOOG]` |
| **String** | `RAY_STR` | 13 | 16 bytes per element | `""` (empty string) | `"hello"` |

!!! note "Null handling"
    Nulls are sentinel-encoded directly in the data array (`INT64_MIN` for `i64`, `NaN` for `f64`, the type-correct reserved value otherwise), not a separate bitmap; the `HAS_NULLS` attribute is a fast "may contain nulls" hint. Parseable null literals are `0Nh` (i16), `0Ni` (i32), `0Nl` (i64), `0Nf` (f64), `0Nd` (date), `0Nt` (time), and `0Np` (timestamp). `RAY_BOOL` and `RAY_U8` have no null value. `RAY_SYM`'s null is the empty symbol `'` and `RAY_STR`'s is the empty string `""`: `(nil? ')` and `(nil? "")` are `true`. Use `nil?` to test for null.

!!! note "Note"
    RAY_SYM and RAY_STR are both string types but serve different purposes. Symbols are dictionary-encoded (interned integers) — ideal for categorical data with repeated values like stock tickers or country codes. Strings are variable-length and stored inline or in a pool — ideal for free-form text.

## Atoms vs Vectors

Every `ray_t` object has a `type` field (a signed 8-bit integer). The sign distinguishes atoms from vectors:

- **Negative type** — atom (scalar). A single value stored directly in the 32-byte header. For example, `-RAY_I64` (-5) is a 64-bit integer atom.
- **Positive type** — vector. A typed array of elements stored in the `data[]` flexible array member following the header.
- **Zero** — `RAY_LIST`. A boxed heterogeneous container of `ray_t*` pointers.

```lisp
; Atom: single value, negative type internally
ray> 42
42

; Vector: typed array, positive type internally
ray> [1 2 3]
[1 2 3]

; Check with meta
ray> (meta 42)
{type:i64}
ray> (meta [1 2 3])
{type:I64 len:3}
```

## The ray_t Header

Every Rayforce object begins with a 32-byte `ray_t` header. This is the fundamental building block — atoms, vectors, lists, tables, and functions all start with this structure.

### Byte Layout

```c
/*
 * ray_t — 32-byte block header (union)
 *
 * Bytes  0-15:  Overlay zone (usage depends on type)
 *   Vectors:    aux[16]      — type-specific auxiliary bytes
 *   Slices:     slice_parent + slice_offset
 *   SYM vecs:   ext_nullmap  + sym_dict
 *   STR vecs:   str_ext_null + str_pool
 *
 * Bytes 16-19:  Metadata
 *   [16] mmod   — 0=heap, 1=file-mmap
 *   [17] order  — buddy block order (size = 2^order)
 *   [18] type   — negative=atom, positive=vector, 0=LIST
 *   [19] attrs  — attribute flags (SLICE, HAS_NULLS, ARENA, etc.)
 *
 * Bytes 20-23:  rc — reference count (0=free)
 *
 * Bytes 24-31:  Value / length
 *   Atoms:      b8, u8, i16, i32, i64, f64, obj, sdata[7]
 *   Vectors:    len (element count)
 *
 * Byte  32+:    data[] — element storage (flexible array member)
 */
```

### Attribute Flags

The `attrs` byte carries bit flags that modify behavior:

| Flag | Bit | Meaning |
|---|---|---|
| `RAY_ATTR_GRAPH` | `0x02` | `-RAY_I64` atom holds a CSR graph handle |
| `RAY_ATTR_HNSW` | `0x04` | `-RAY_I64` atom holds an HNSW handle |
| `RAY_ATTR_HAS_INDEX` | `0x08` | Vector carries an attached accelerator index |
| `RAY_ATTR_SLICE` | `0x10` | Vector is a zero-copy slice of a parent |
| `RAY_ATTR_SORTED` | `0x20` | Vector is known to be in non-descending order |
| `RAY_ATTR_HAS_NULLS` | `0x40` | Vector may contain null sentinels (hint) |
| `RAY_ATTR_ARENA` | `0x80` | Arena-allocated; retain/release are no-ops |

## Type Casting

The `as` function converts between types. Pass the target type as a symbol and the value to cast:

```lisp
; Integer vector to float
ray> (as 'F64 [1 2 3])
[1.0 2.0 3.0]

; Integer to string
ray> (as 'STR 42)
"42"

; Float to integer (truncates)
ray> (as 'I64 [1.5 2.7 3.9])
[1 2 3]

; Date from string
ray> (as 'DATE "2024.01.15")
2024.01.15
```

### Supported Casts

| From | To | Notes |
|---|---|---|
| Any numeric | Any numeric | Widening is lossless; narrowing truncates |
| BOOL | I64, F64 | `true` → 1, `false` → 0 |
| I64, F64 | BOOL | Non-zero → `true` |
| Any scalar | STR | Formatted string representation |
| STR | I64, F64, DATE, TIME, TIMESTAMP | Parsed from standard format |
| SYM | STR | Resolves interned string |
| STR | SYM | Interns the string |
| DATE | TIMESTAMP | Midnight on that date |
| TIMESTAMP | DATE | Truncates to day |

## Temporal Types

Rayforce has three temporal types with different precision and epoch conventions:

### RAY_DATE

Stored as a 32-bit signed integer counting **days since 2000.01.01**. This gives a range of roughly 5.8 million years in either direction. A result past that range keeps its low 32 bits, the same for an atom and for a vector cell.

```lisp
ray> 2024.01.15
2024.01.15

; Arithmetic works in days
ray> (+ 2024.01.15 30)
2024.02.14
```

### RAY_TIME

Stored as a 32-bit signed integer counting **milliseconds since midnight**. A time of day uses 0 to 86,399,999 ms, but values past 24 hours and negative values are valid, up to about ±596 hours (±2^31 ms). A result past that range keeps its low 32 bits, the same for an atom and for a vector cell: `(* 25 24:00:00.000)` is `-593:02:47.296`.

```lisp
ray> 09:30:00.000
09:30:00.000

; Arithmetic works in milliseconds
ray> (+ 09:30:00.000 60000)
09:31:00.000
```

### RAY_TIMESTAMP

Stored as a 64-bit signed integer counting **nanoseconds since 2000-01-01** (the Rayforce epoch — matches the CSV loader's parser and the runtime's arithmetic lifts). Nanosecond precision covers roughly the range 1708 through 2292.

```lisp
ray> 2024.01.15D09:30:00.000000000
2024.01.15D09:30:00.000000000
```

## Symbols (RAY_SYM)

Symbols are dictionary-encoded strings. Each unique string is interned once in a global symbol table and assigned an integer ID. Vectors of symbols store only these compact IDs, making them extremely space-efficient for categorical data.

### Adaptive Width

Symbol vectors automatically choose the narrowest integer width needed:

- **8-bit** — up to 255 unique symbols
- **16-bit** — up to 65,535 unique symbols
- **32-bit** — up to ~4 billion unique symbols
- **64-bit** — unlimited

```lisp
; Symbol literal (tick prefix)
ray> 'AAPL
'AAPL

; Symbol vector (auto-detected from unquoted identifiers)
ray> [AAPL GOOG MSFT AAPL GOOG]
[AAPL GOOG MSFT AAPL GOOG]
```

## Strings (RAY_STR)

Variable-length strings use a 16-byte `ray_str_t` element per row. Short strings (12 bytes or fewer) are stored inline. Longer strings are stored in a per-vector pool with a 4-byte prefix cached inline for fast comparison rejection.

### Inline vs Pool Storage

| String Length | Storage | Access |
|---|---|---|
| 0 – 12 bytes | Inline in `ray_str_t` | Direct, no indirection |
| 13+ bytes | Per-vector pool | 4-byte prefix inline + pool offset |

```lisp
; Short string (stored inline)
ray> "hello"
"hello"

; String vector
ray> (list "short" "a longer string here")
```

## C API

Atom constructors return a `ray_t*` with the value stored directly in the header:

```c
// Atom constructors
ray_t* b = ray_bool(true);
ray_t* n = ray_i64(42);
ray_t* f = ray_f64(3.14);
ray_t* d = ray_date(8780);    // days since 2000.01.01
ray_t* s = ray_str("hello", 5);
ray_t* y = ray_sym(sym_id);   // interned symbol ID

// Type inspection
ray_type(n);       // -RAY_I64  (negative = atom)
ray_is_atom(n);    // true
ray_is_vec(n);     // false
```

Vector constructors allocate a header plus contiguous element storage:

```c
// Create typed vectors
ray_t* v = ray_vec_new(RAY_I64, 1024);
v = ray_vec_append(v, &(int64_t){42});

// From raw data
int64_t data[] = {1, 2, 3};
ray_t* v2 = ray_vec_from_raw(RAY_I64, data, 3);

// Adaptive-width symbol vector
ray_t* sv = ray_sym_vec_new(1, 1024);  // 8-bit width

// String vector
ray_t* strs = ray_vec_new(RAY_STR, 64);
strs = ray_str_vec_append(strs, "hello", 5);
```
