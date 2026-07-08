# String Operations

Complete reference for string manipulation in Rayforce — from basic transforms to pattern matching, covering both RAY_SYM and RAY_STR column types.

## RAY_SYM vs RAY_STR

Rayforce provides two distinct string column representations, each optimized for different workloads. Choosing the right type is critical for performance.

### RAY_SYM — Dictionary-Encoded Symbols

`RAY_SYM` columns store strings as integer indices into a global intern table. Ideal for **low-cardinality** categorical data (country codes, status flags, product categories).

- **Adaptive-width indices** — 8, 16, 32, or 64-bit integers depending on dictionary size
- **Equality comparison** is a single integer compare — O(1)
- **Group-by** on SYM columns uses direct index lookup instead of hashing
- **Memory efficient** — each row stores only 1–8 bytes regardless of string length
- **Global intern table** — symbols are shared across all columns and tables

```lisp
; Create a table with a SYM column
(set t (table [region name]
  (list [US EU US APAC] ["Alice" "Bob" "Ada" "Chen"])))
(type (at t 'region))
```

### RAY_STR — Variable-Length Strings

`RAY_STR` columns store variable-length strings with a hybrid inline/pool layout. Best for **high-cardinality** or **unique** text data (names, descriptions, URLs).

- **SSO (Small String Optimization)** — strings of 12 bytes or fewer are stored inline in the 16-byte `ray_str_t` element, requiring zero indirection
- **Pool storage** — strings longer than 12 bytes are written to a per-vector pool; the element stores a 4-byte offset and 4-byte length, plus a **4-byte prefix** copied inline for fast comparison rejection
- **Fast comparison rejection** — the 4-byte prefix allows most unequal comparisons to short-circuit without following the pool pointer
- **Per-vector pool** — each RAY_STR vector has its own pool; `col_propagate_str_pool()` shares pools between source and destination during execution

```lisp
; STR columns are used for unique/high-cardinality text
(set names ["Alice" "Bob" "Charlie"])
(type names)
```

!!! note "When to use which?"
    Use `RAY_SYM` for columns with fewer than ~65K unique values (status codes, categories, tickers). Use `RAY_STR` for free-text, names, addresses, or any column where most values are unique. The CSV reader auto-detects: columns with a high repeat ratio become SYM, others become STR.

## Null Propagation

All string operations in Rayforce follow strict null propagation semantics:

- **Null input produces null output** — if any required input row is null, the output row is null
- **CONCAT is null if any argument is null**
- Null propagation applies uniformly to both RAY_SYM and RAY_STR columns
- Null state is carried through the execution pipeline per morsel (1024 elements)

In the C API DAG, null propagation is handled automatically per morsel. String transformation opcodes (STRLEN, UPPER/LOWER/TRIM, SUBSTR, REPLACE, CONCAT) propagate nulls: null input rows produce null output rows. CONCAT is null if any argument is null.

## String Functions

Rayfall exposes the common string transforms as direct builtins. On vector inputs, `upper`, `lower`, `trim`, `substr`, and `replace` are lazy-aware and use the same morsel-based DAG opcodes that query expressions use. `str-find` uses the thread pool for large direct vector searches. `ilike` remains a C API DAG opcode.

### concat

**`(concat a b)`** — binary

Concatenates two string arguments. Works on string atoms and vectors element-wise.

```lisp
(concat "hello" " world")
```

### like

**`(like str pattern)`** — binary · element-wise

Case-sensitive glob pattern matching. Returns a boolean (or boolean vector for vector input). Supports `*` (match any sequence of characters) and `?` (match any single character). Works on both RAY_SYM and RAY_STR columns.

```lisp
(like "hello world" "*world")

(like "hello world" "hello*")

(select {from: t where: (like name "A*")})
; Returns all rows where name starts with "A"
```

### upper / lower / trim

**`(upper x)`**, **`(lower x)`**, **`(trim x)`** — unary · atom/vector · lazy-aware

Transforms string or symbol atoms and vectors. `trim` removes leading and trailing whitespace.

```lisp
(upper "Abc42")
(lower 'AbC)
(trim [" a " "\tb\t" ""])
```

### substr

**`(substr str start len)`** — variadic · atom/vector · lazy-aware

Extracts a substring using a 1-based start position and a length. Start values below 1 clamp to the first byte. Negative lengths consume through the end. `start` and `len` may be integer atoms or `I64`/`I32` vectors for vector inputs.

```lisp
(substr "abcdef" 2 3)
; "bcd"

(substr ["abcdef" "xyz"] [1 2] [3 9])
; ["abc" "yz"]
```

### replace

**`(replace str from to)`** — variadic · atom/vector · lazy-aware

Replaces all occurrences of `from` with `to`. The `from` and `to` arguments must be string or symbol atoms.

```lisp
(replace "banana" "na" "NA")
; "baNANA"

(replace (upper ["a-b" "c-d"]) "-" "_")
; ["A_B" "C_D"]
```

### split

**`(split str delimiter)`** — binary · element-wise

Splits each string element by the given delimiter and returns a list of string vectors. Each element in the result is a vector of the split parts. Null input produces null output.

```lisp
(split "a,b,c" ",")

(split "hello world" " ")
```

### str-find

**`(str-find str needle)`** — binary · atom/vector/list

Returns the first 0-based byte index of `needle` in each string or symbol value. A missing substring returns `0Nl`, matching the collection `find` convention for "not found". Empty `needle` returns `0`. Large string/symbol vectors run through the thread pool and poll cancellation between morsels.

```lisp
(str-find "banana" "na")
; 2

(str-find ["banana" "cab" ""] "a")
; [1 1 0Nl]
```

### str-join

**`(str-join items delimiter)`** — binary

Joins string or symbol atoms, vectors, or lists using a string/symbol delimiter. The result is a string atom. Empty inputs produce `""`; a single item is returned without adding the delimiter.

```lisp
(str-join ["2026" "04" "16"] "-")
; "2026-04-16"

(str-join (list "a" 'b "c") "/")
; "a/b/c"
```

### format

**`(format fmt ...args)`** — variadic

Formats values into a string using a format template. Each `%` placeholder is replaced with the next stringified argument in order. Useful for building display strings or log messages.

```lisp
ray> (format "Hello, %!" "world")
"Hello, world!"

ray> (format "% + % = %" 1 2 3)
"1 + 2 = 3"
```

## String Operations in the DAG

When using the C API, string operations are available as DAG opcodes. These are fused into morsel-driven execution alongside arithmetic and comparison operations.

| Opcode | C API | Description |
|---|---|---|
| `OP_UPPER` | `ray_upper(g, a)` | Uppercase transform |
| `OP_LOWER` | `ray_lower(g, a)` | Lowercase transform |
| `OP_STRLEN` | `ray_strlen(g, a)` | String byte length |
| `OP_TRIM` | `ray_trim_op(g, a)` | Strip leading/trailing whitespace |
| `OP_SUBSTR` | `ray_substr(g, str, start, len)` | Extract substring by position |
| `OP_REPLACE` | `ray_replace(g, str, from, to)` | Replace all occurrences |
| `OP_CONCAT` | `ray_concat(g, args, n)` | Concatenate N strings |
| `OP_LIKE` | `ray_like(g, input, pattern)` | Case-sensitive glob pattern match (`*`/`?` wildcards) |
| `OP_ILIKE` | `ray_ilike(g, input, pattern)` | Case-insensitive glob pattern match |

### C API Example

```c
/* Filter rows where upper(name) LIKE "A*" and compute strlen */
ray_graph_t* g = ray_graph_new(table);

ray_op_t* name    = ray_scan(g, "name");
ray_op_t* up_name = ray_upper(g, name);
ray_op_t* pattern = ray_const_str(g, "A*", 2);
ray_op_t* pred    = ray_like(g, up_name, pattern);

ray_op_t* filt_name = ray_filter(g, name, pred);
ray_op_t* name_len  = ray_strlen(g, filt_name);

/* Execute — upper, like, filter, strlen all fused into one morsel pass */
ray_t* result = ray_execute(g, ray_optimize(g, name_len));
```

## String Pool Internals

Understanding the internal layout helps explain performance characteristics of string operations.

### ray_str_t Element Layout (16 bytes)

| Bytes | Inline (SSO) | Pool Reference |
|---|---|---|
| 0–3 | String data [0..3] | 4-byte prefix (first 4 bytes of string) |
| 4–7 | String data [4..7] | Pool offset (uint32_t) |
| 8–11 | String data [8..11] | String length (uint32_t) |
| 12–15 | Length + flag | Length + flag (high bit = 1 for pool) |

!!! note "SSO threshold"
    Strings of 12 bytes or fewer are stored entirely within the 16-byte element — no heap allocation, no pointer chase. The majority of real-world strings (tickers, codes, short names) benefit from this optimization. Access via `ray_str_vec_get()` returns a pointer to the inline data or pool data transparently.

### Hash and Comparison

String hashing uses `ray_str_t_hash()` which operates directly on the element bytes. Comparison via `ray_str_t_cmp()` / `ray_str_t_eq()` first compares the 4-byte prefix for fast rejection, then falls through to a full byte comparison only when prefixes match. This makes hash joins and group-by on string columns significantly faster than naive approaches.

### Dictionary-Encoded Symbol Width

`RAY_SYM` columns use adaptive-width integer indices to minimize memory:

| Dictionary Size | Index Width | Bytes per Row |
|---|---|---|
| ≤ 255 | 8-bit | 1 |
| ≤ 65,535 | 16-bit | 2 |
| ≤ 4,294,967,295 | 32-bit | 4 |
| Larger | 64-bit | 8 |

Width is set at column creation via `ray_sym_vec_new(sym_width, capacity)` where `sym_width` is 1, 2, 4, or 8. The CSV reader picks the narrowest width that fits the observed cardinality.
