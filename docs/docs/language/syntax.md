# Syntax & Types

Rayfall is a Lisp-like query language with prefix notation, rich scalar types, columnar vectors, and first-class tables. The parser produces `ray_t` objects directly with no separate AST.

## Atoms

Atoms are scalar values. Rayfall supports a wide range of types, each with a distinct literal syntax.

### Integers

64-bit signed integers by default. Suffixed variants available for narrower types.

```lisp
42          ; i64 (default)
-17         ; negative
0           ; zero
1000000     ; no separators needed
```

### Floats

64-bit IEEE 754 double-precision floating point.

```lisp
3.14        ; standard float
-0.5        ; negative float
1e6         ; scientific notation
2.5e-3      ; small value
```

### Booleans

```lisp
true        ; boolean true
false       ; boolean false
```

### Symbols

Symbols are interned identifiers used for column names, dictionary keys, and categorical data. Prefix with a single quote to create a literal symbol.

A symbol literal runs over letters, digits, `_`, `.` and `-`. A symbol, keyword, name or number cannot be directly followed by a quote, a colon, or another name or number character: `['a:1]` is a parse error rather than the two symbols `a` and `1`, and `0Na` is not `0` followed by `Na`. A symbol that must contain a colon is built from a string, `(as 'symbol "a:1")`.

```lisp
'AAPL       ; symbol atom
'price      ; used as column reference
'hello      ; any alphanumeric + hyphens
```

### Strings

Double-quoted character sequences. Two internal representations: short strings (up to 12 bytes) stored inline, longer strings in a per-vector pool.

```lisp
"hello"             ; inline short string
"hello world!"      ; still inline (12 bytes)
"a longer string"   ; pool-allocated
```

### Dates

Date literals use dot-separated year.month.day format. Stored as days since 2000-01-01 (i32).

```lisp
2024.01.15  ; January 15, 2024
2023.12.31  ; December 31, 2023
```

The month must be `1`-`12` and the day `1`-`31`; anything outside those
ranges is a parse error (e.g. `2024.13.01` or `2024.01.00`). A day past the
end of its month is normalized by the calendar rather than rejected, so
`2024.02.30` parses as `2024.03.01`.

### Times

Time-of-day literals in HH:MM:SS.mmm format. Stored as milliseconds since midnight (i32).

```lisp
09:30:00.000  ; 9:30 AM
14:15:30.500  ; 2:15:30.5 PM
```

Hours must be `0`-`23`, minutes and seconds `0`-`59`; out-of-range
components are a parse error.

### Timestamps

Full date+time with nanosecond precision. Stored as nanoseconds since 2000-01-01 (i64). Literal form uses `D` as the date/time separator and requires a fractional-seconds suffix:

```lisp
2024.01.15D09:30:00.000            ; date + time (ms)
2024.01.15D09:30:00.000000000      ; date + time (ns)
```

The date and time parts follow the same range rules as above. Because the
value is a 64-bit nanosecond count from the epoch, timestamps only span
roughly ±292 years around 2000 (about 1708 to 2292); a year outside that
range is a parse error.

### GUIDs

128-bit globally unique identifiers.

```lisp
(guid 1)   ; generate one GUID
```

### Null Values

Nulls are sentinel-encoded directly in the column payload — `INT64_MIN`
for `i64`, `NaN` for `f64`, and the type-correct reserved value for each
other type (there is no separate null bitmap). The `HAS_NULLS` attribute
is a fast "may contain nulls" hint. Typed null literals produce the
sentinel for their type:

```lisp
0Nl   ; i64 null
0Ni   ; i32 null
0Nh   ; i16 null
0Nf   ; f64 null
0Nd   ; date null
0Nt   ; time null
0Np   ; timestamp null
(nil? 0Nl)   ; true
```

Symbols have no typed null literal (there is no `0Ns`).

`nil?` is element-wise: given a vector or a list it returns a `B8` vector with
one entry per element, so `(where (nil? v))` and `(sum (as 'I64 (nil? v)))`
work without a `map`. An atom answers for itself, and so does any other
container (a table or a dict is never null). Inside `select` and `update`
expressions, `(nil? col)` lowers to the DAG null-check opcode, which gives the
same per-row answer and can be used directly in `where:` predicates and
projections.

```lisp
(nil? [1 0N 3])   ; [false true false]
(nil? [a ' b])    ; [false true false]  — the empty symbol is SYM's null
```

## Vectors

Vectors are homogeneous, typed, columnar arrays. Created with square brackets. The type is inferred from the first element.

```lisp
[1 2 3 4 5]         ; i64 vector
[1.5 2.7 3.9]       ; f64 vector
[true false true]   ; boolean vector
[AAPL GOOG MSFT]    ; symbol vector
["hello" "world"]   ; string vector
```

Vector operations are morsel-driven, processing 1024 elements at a time for cache efficiency.

### Vector Arithmetic

All arithmetic operators auto-map over vectors (marked `FN_ATOMIC`):

```lisp
(+ [1 2 3] [10 20 30])   ; [11 22 33]
(* [1 2 3] 10)            ; [10 20 30] — scalar broadcast
```

## Lists

Lists are heterogeneous collections of vectors. Created with the `list` function. Used as the data component of tables.

```lisp
(list [1 2 3] [A B C])   ; list of two vectors
```

!!! note "Nesting depth"
    The parser caps recursive nesting of container literals and calls at
    1024 levels. A pathologically deep input (e.g. thousands of nested
    `[` or `(`) is rejected with `parse: nesting too deep, limit 1024`
    rather than overflowing the stack. Real queries never approach this.

## Tables

Tables are the core data structure in Rayforce. A table is a vector of column names paired with a list of column vectors. All column vectors must have the same length.

```lisp
; Create a table with explicit column names
(set trades (table
  [sym price size]
  (list
    [AAPL GOOG MSFT]
    [150.5 2800.0 300.2]
    [100 50 200])))

; Access column names
(key trades)     ; [sym price size]

; Access column data
(value trades)   ; list of 3 vectors
```

## Dictionaries

Dictionaries map keys to values. Created with the `dict` function or with `{key: value}` syntax in query contexts.

```lisp
(set d (dict [a b c] [1 2 3]))
(get d 'a)    ; 1
(key d)       ; [a b c]
(value d)     ; [1 2 3]
```

## Function Calls

Rayfall uses prefix (Polish) notation. Every expression is either an atom or a parenthesized list where the first element is the function:

```lisp
(+ 1 2)           ; 3
(* (+ 1 2) 3)     ; 9 — nested
(sum [1 2 3])     ; 6
(count [1 2 3])   ; 3
```

### Function Types

Built-in functions fall into three arity categories:

| Type | Arguments | Examples |
|---|---|---|
| **Unary** | Exactly 1 | `sum`, `count`, `not`, `neg`, `type` |
| **Binary** | Exactly 2 | `+`, `-`, `set`, `take`, `at` |
| **Variadic** | 1 or more | `if`, `do`, `fn`, `select`, `list` |

### Function Flags

| Flag | Behavior |
|---|---|
| `FN_ATOMIC` | Auto-maps element-wise over vectors. `(+ [1 2] [3 4])` yields `[4 6]`. |
| `FN_AGGR` | Aggregation function. Reduces a vector to a scalar. `(sum [1 2 3])` yields `6`. |
| `FN_SPECIAL_FORM` | Arguments are not evaluated before being passed. Used by `set`, `if`, `fn`, `select`. |

## Quoting

The single quote `'` prevents evaluation, creating a symbol atom. Useful for column references and dictionary keys:

```lisp
'price            ; symbol, not a variable lookup
(quote (+ 1 2))   ; returns the unevaluated list (+ 1 2)
(quote price)     ; a bare name becomes a literal symbol, equal to 'price
```

`quote` returns its argument unevaluated. When the argument is a bare name, the result is a literal symbol — `(quote name)` is equal to the tick form `'name`. Such a literal symbol, when it names a from-table column, resolves to that column during query evaluation. This is how the dynamic-query idiom works: a query assembled as data evaluates `(quote name)` to the literal symbol `'name` during construction, and that symbol resolves to the column when the query runs (see [IPC](../storage/ipc.md)). Inside a query, the tick form `'name` and a bare `name` resolve the same way.

## Comments

Line comments start with a semicolon and extend to the end of the line:

```lisp
; This is a comment
(+ 1 2)  ; inline comment
```

## Control Flow

### Conditional: `if`

Evaluates the condition and returns the true or false branch. Supports `if/then/else` chaining:

```lisp
(set x 75)
(if (> x 0) "positive" "non-positive")

; Multi-branch
(if (> x 100) "high"
    (> x 50)  "mid"
              "low")
```

### Sequential Execution: `do`

Evaluates multiple expressions in order, returning the last result:

```lisp
(do
  (set x 10)
  (set y 20)
  (+ x y))    ; 30
```

### Variable Binding: `set` and `let`

```lisp
(set x 42)        ; global binding
(let y (+ x 1))   ; local binding
```

### Dotted Namespaces

A symbol whose name contains one or more `.` is a *dotted* symbol — it names a path through nested dictionaries rather than a single global binding. `set` auto-creates the intermediate dicts on write, and read/delete traverse them with copy-on-write semantics so other references to the enclosing dict see their old value.

```lisp
; Write a nested namespace — intermediate dicts are created automatically
(set math.pi 3.14159)
(set math.e  2.71828)
(set cfg.db.host "localhost")
(set cfg.db.port 5432)

; Read walks the path segment by segment
math.pi          ; 3.14159
cfg.db.host      ; "localhost"

; Deleting a leaf cascades: cfg.db becomes empty, then cfg.db is itself removed
(del cfg.db.host)
(del cfg.db.port)
(nil? (resolve 'cfg.db))   ; true
```

When a dotted path lands on a **temporal** value (DATE / TIME / TIMESTAMP atom or vector), the trailing segment dispatches through the temporal extraction API instead of looking for a dict key. This lets you reach calendar fields uniformly:

```lisp
(set d 2024.03.15)
d.yyyy           ; 2024
d.mm             ; 3
d.dd             ; 15
d.dow            ; 5   (ISO: Mon=1..Sun=7)

; Vector of dates — the whole column is lifted
(set ds [2024.01.01 2024.06.15 2024.12.31])
ds.yyyy          ; [2024 2024 2024]
ds.doy           ; [1 167 366]

; Inside a table, col.yyyy resolves against the column vector
(set ts_table (table [Ts]
  (list [2024.01.01D10:00:00.000 2024.01.01D11:00:00.000])))
(select {from: ts_table by: Ts.date})   ; group by day
(select {from: ts_table by: Ts.hh})     ; group by hour of day
```

Recognised temporal segments: `yyyy`, `mm`, `dd`, `hh`, `minute`, `ss`, `dow`, `doy`, plus the two truncations `date` (drop time-of-day) and `time` (drop date). Null input rows propagate to null output rows — the null sentinel bit pattern is not decoded as a bogus calendar value.

### Reserved `.*` Namespace

Symbols whose name starts with `.` are a reserved system namespace populated at startup by builtin registration. Typing one of these at the REPL returns the namespace dict for introspection:

```lisp
.sys     ; {gc:<.sys.gc> exec:<.sys.exec> info:<.sys.info> mem:<.sys.mem> build:<.sys.build>}
.os      ; {getenv:<.os.getenv> setenv:<.os.setenv>}
.ipc     ; {open:<.ipc.open> close:<.ipc.close> send:<.ipc.send>}
.csv     ; {read:<.csv.read> write:<.csv.write>}
```

Every `.`-prefixed name is protected: `set`, `let`, lambda parameters, and `del` all refuse such names with `error: reserve`. This keeps user code from shadowing the builtin surface, regardless of where it's bound.

```text
(set .os.foo 1)        ; error: reserve
(let .sys.gc 99)       ; error: reserve
((fn [.sys.gc] .sys.gc) 7)  ; error: reserve (lambda parameter name)
(del .sys.gc)          ; error: reserve: cannot delete reserved binding
```

### Error Handling: `try` / `raise`

```lisp
(try
  (raise "oops")     ; throws an error
  (fn [e] "caught")) ; handler receives error → "caught"

; Unhandled raise:
; (raise "custom error") ; error: domain
```

## Lambdas & the VM

User-defined functions are created with `fn`. Lambdas compile lazily to bytecode and run in a stack-based computed-goto VM (`ray_vm_t`) with a 1024-slot program stack and return stack.

```lisp
; Named function
(set square (fn [x] (* x x)))
(square 5)    ; 25

; Multi-expression body
(set clamp (fn [x lo hi]
  (if (< x lo) lo
      (> x hi) hi
              x)))

; Anonymous lambda passed to map
(map (fn [x] (* x 2)) [1 2 3])   ; [2 4 6]
```

The VM supports trap frames for `try`/`raise` error handling, ensuring exceptions unwind cleanly through compiled code.

## Select & Update

The `select` and `update` builtins bridge to the Rayforce DAG executor. They accept a dictionary of options:

### select

```lisp
; Basic filter
(select {from: trades  where: (> price 100)})

; Project specific columns with expressions
(select {from: trades
         sym: sym  notional: (* price size)})

; Group by with aggregation
(select {from: trades
         by:   {sym: sym}
         avg_price: (avg price)
                total_size: (sum size)})
```

### update

```lisp
; Add a computed column
(update {from: trades
          notional: (* price size)})
```

### insert / upsert

`insert` takes the target table and a table of new rows, and returns the
combined table (the two-argument form does not mutate its argument):

```lisp
; Append new rows — returns a table with the extra rows
(insert trades
        (table [sym price size]
               (list [TSLA] [250.0] [300])))
```

To mutate a named global in place, pass the target as a quoted symbol —
`(insert 'trades newrows)` — or use `upsert` with an explicit key count,
`(upsert 'trades 1 newrows)`.

## C API

Rayforce exposes a single public header: `include/rayforce.h`. The core abstraction is `ray_t` — a 32-byte block header. Every object (atom, vector, list, table) is a `ray_t` with data following at byte 32.

### Key Types

| Type | Description |
|---|---|
| `ray_t` | 32-byte universal block header for all objects |
| `ray_err_t` | Error code return type |
| `ray_str_t` | 16-byte string element (inline or pooled) |
| `ray_csr_t` | CSR graph edge storage |
| `ray_rel_t` | Graph relationship (forward + reverse CSR) |
| `ray_arena_t` | Bump allocator for bulk allocations |
| `ray_vm_t` | Bytecode VM for compiled lambdas |

### Error Handling

```c
ray_t* result = ray_eval_str("(+ 1 2)");
if (RAY_IS_ERR(result)) {
    // handle error
}
// RAY_ERR_PTR() to create error pointers
```

### Memory Management

Never use `malloc`/`free`. Use the Rayforce allocator:

```c
ray_t* obj = ray_alloc(size);    // general allocation
ray_release(obj);                 // decrement refcount, free if zero
ray_retain(obj);                  // increment refcount
ray_t* copy = ray_cow(obj);      // copy-on-write
```

## DAG & Execution

The execution pipeline builds a lazy DAG, optimizes it, then executes with fused morsel-driven processing:

```c
// 1. Build lazy DAG
ray_t* g = ray_graph_new(df);
ray_t* filtered = ray_filter(g, predicate);
ray_t* projected = ray_project(g, filtered, cols);

// 2. Execute (optimizer runs automatically)
ray_t* result = ray_execute(g, projected);
```

### Optimizer Passes

1. **Type inference** — propagate types through the DAG
2. **Constant folding** — evaluate compile-time-known expressions
3. **SIP** (Sideways Information Passing) — propagate selection bitmaps backward through expand chains
4. **Factorize** — avoid materializing cross-products with factorized vectors
5. **Predicate pushdown** — move filters closer to data sources
6. **Filter reorder** — cheapest filters first
7. **Fusion** — merge adjacent operations into single morsel loops
8. **DCE** (Dead Code Elimination) — remove unused DAG nodes

## CSR Storage

Rayforce stores graph edges in double-indexed Compressed Sparse Row (CSR) format: one forward index (source to destination) and one reverse index (destination to source). Both indices are built simultaneously.

```c
// Build CSR from edge list
ray_csr_t csr;
ray_csr_build(&csr, src_ids, dst_ids, n_edges);

// Persist to disk
ray_csr_save(&csr, "edges");

// Memory-map for zero-copy access
ray_csr_mmap(&csr, "edges");
```

## Graph Algorithms

Available as DAG opcodes, all integrated into the same morsel-driven pipeline:

| Opcode | Algorithm | Description |
|---|---|---|
| `OP_EXPAND` | 1-Hop Expand | Follow edges one step from source nodes |
| `OP_VAR_EXPAND` | BFS | Variable-length path expansion (breadth-first) |
| `OP_SHORTEST_PATH` | Shortest Path | Single-source shortest paths |
| `OP_ASTAR` | A* | Heuristic-guided shortest path |
| `OP_K_SHORTEST` | Yen's K-Shortest | K shortest loopless paths |
| `OP_WCO_JOIN` | LFTJ | Worst-case optimal join (Leapfrog Triejoin) |
| `OP_BETWEENNESS` | Brandes | Betweenness centrality |
| `OP_CLOSENESS` | Closeness | Closeness centrality |
| `OP_CLUSTER_COEFF` | Clustering | Local clustering coefficients |
| `OP_RANDOM_WALK` | Random Walk | Random walks on graph |
| `OP_MST` | Kruskal | Minimum spanning tree |

## Pipeline & Optimizer

The full execution pipeline:

```
Rayfall source
  |  parse (ASCII dispatch table, recursive descent)
  v
ray_t objects (no separate AST)
  |  ray_eval() / bytecode VM
  v
Lazy DAG construction
  |  ray_graph_new() -> ray_scan/ray_add/ray_filter/...
  v
Optimizer (8 passes)
  |  type inference -> constant fold -> SIP -> factorize
  |  -> predicate pushdown -> filter reorder -> fusion -> DCE
  v
Fused morsel-driven executor
  |  bytecode over register slots, 1024 elements per morsel
  v
Result (ray_t)
```

## Memory Model

- **Buddy allocator** with thread-local arenas for contention-free allocation
- **Slab cache** for small, frequently-allocated objects
- **COW ref counting** — `ray_cow()` returns a private copy only when the refcount exceeds 1
- **Arena (bump) allocator** (`ray_arena_t`) for bulk short-lived allocations; blocks carry `RAY_ATTR_ARENA`, making retain/release no-ops
- **Per-VM heaps** — each heap carries a `heap_id` (u16); cross-heap frees enqueue to a lock-free LIFO, reclaimed via `ray_heap_flush_foreign()`

## Files & Partitions

- **Column files** — native binary format for vectors and CSR graphs, supports mmap
- **Sym table** — global string intern table, arena-backed, append-only persistence with file locking
- **CSV loader** — mmap-based, parallel parse, automatic type inference, null handling, sym merge
- **File I/O** — cross-platform locking (flock/LockFileEx), fsync, atomic rename
