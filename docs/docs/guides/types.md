# Type Casting & Coercion

!!! note "On this page"

    1. [The `as` Function](#as-function)
    2. [The `type` Function](#type-function)
    3. [Implicit Coercion Rules](#implicit-coercion)
    4. [String ↔ Type Conversions](#string-conversions)
    5. [Null Semantics in Casting](#null-semantics)
    6. [Symbol vs String](#sym-vs-str)
    7. [Date/Time Types](#date-time)
    8. [GUID Type](#guid)
    9. [sym-name Function](#sym-name)
    10. [Practical Examples](#practical-examples)

## 1. The `as` Function { #as-function }

The `as` function performs explicit type conversion. It takes a type symbol and a value, returning the value cast to that type.

```text
(as 'type-symbol value)
```

**Supported type symbols:**

| Symbol | Type | Description |
|---|---|---|
| `'i16` | 16-bit integer | Short integer |
| `'i32` | 32-bit integer | Standard integer |
| `'i64` | 64-bit integer | Long integer (default) |
| `'u8` | unsigned 8-bit | Byte values |
| `'f64` | 64-bit float | Double precision (default) |
| `'b8` | Boolean | true / false |
| `'str` | String | Variable-length text |
| `'sym` | Symbol | Dictionary-encoded string |
| `'date` | Date | Days since 2000-01-01 |
| `'time` | Time | Milliseconds since midnight |
| `'timestamp` | Timestamp | Nanoseconds since 2000-01-01T00:00:00 |
| `'guid` | GUID | 16-byte unique identifier |

**Numeric casts:**

```lisp
; Integer to float
(as 'f64 42)          ; 42.0

; Float to integer (truncates)
(as 'i64 3.14)        ; 3

; Narrow to wide integer
(as 'i16 100)         ; 100i16
```

**Boolean casts:**

```lisp
; Integer to boolean
(as 'b8 1)          ; true
(as 'b8 0)          ; false

; Boolean to integer
(as 'i64 true)        ; 1
(as 'i64 false)       ; 0
```

**String and symbol casts:**

```lisp
; String to symbol
(as 'sym "hello")    ; 'hello

; Symbol to string
(as 'str 'hello)     ; "hello"
```

**Temporal casts from strings:**

```lisp
; Parse date from ISO string
(as 'date "2024.01.15")         ; 2024.01.15

; Parse time from string
(as 'time "12:30:00")           ; 12:30:00.000

; Parse timestamp (ISO 8601 input accepted; display uses the `D` separator)
(as 'timestamp "2024-01-15T12:30:00.000")  ; 2024.01.15D12:30:00.000000000
```

**Vector casts:** `as` maps element-wise over vectors.

```lisp
; Cast an integer vector to float
(as 'f64 [1 2 3])     ; [1.0 2.0 3.0]

; Cast a float vector to integer
(as 'i64 [1.5 2.7 3.9])  ; [1 2 3]
```

## 2. The `type` Function { #type-function }

The `type` function returns the type name of a value as a symbol. For vectors, it returns the element type.

```lisp
(type 42)                ; 'i64
(type 3.14)              ; 'f64
(type "hello")           ; 'str
(type 'hello)            ; 'sym
(type true)               ; 'b8
(type [1 2 3])           ; 'I64  (uppercase = vector type)
(type [AAPL GOOG])       ; 'SYM
(type [1.0 2.0])         ; 'F64
(type 2024.01.15)        ; 'date
(type 12:30:00.000)      ; 'time
```

!!! note "Note"

    `type` returns lowercase for atoms (`'i64`, `'f64`, `'sym`, `'b8`) and uppercase for vectors (`'I64`, `'F64`, `'SYM`). The cast function `as` accepts both `'sym` and `'symbol`.

For tables, `type` returns `'TABLE`:

```lisp
(type (table [a] (list [1])))  ; 'TABLE
```

Use `type` to branch on value types at runtime or to validate input before casting.

## 3. Implicit Coercion Rules { #implicit-coercion }

Rayfall performs automatic type promotion in certain contexts. These coercions happen silently — no explicit `as` needed.

!!! note "Coercion rules"

    - **i64 + f64 → f64:** Arithmetic between integers and floats promotes to float.
    - **bool → i64:** Booleans coerce to 1/0 in arithmetic contexts.
    - **i64 → bool:** In boolean contexts (if, cond), 0 is false, non-zero is true.
    - **Narrower → wider:** Comparisons across numeric types promote the narrower operand.
    - **No string coercion:** Strings never implicitly convert to numbers. Use `as` explicitly.

```lisp
; Integer + float promotes to float
(+ 1 2.0)               ; 3.0
(* 3 1.5)               ; 4.5

; Boolean in arithmetic
(+ 10 true)              ; 11
(sum [true false true])   ; 2

; Boolean context
(if 42 "yes" "no")      ; "yes"
(if 0 "yes" "no")       ; "no"

; Cross-type comparison
(> 5 3.0)               ; true  (i64 promoted to f64)

; String + number: ERROR (no implicit coercion)
; (+ "42" 1)  ; type error — use (+ (as 'i64 "42") 1)
```

## 4. String ↔ Type Conversions { #string-conversions }

Strings can be parsed into numeric, temporal, and symbol types with `as`. Conversely, any value can be cast to a string.

```lisp
; Parse integers and floats from strings
(as 'i64 "42")          ; 42
(as 'f64 "3.14")        ; 3.14

; Number to string
(as 'str 42)            ; "42"
(as 'str 3.14)          ; "3.14"

; Parse date from ISO string (YYYY-MM-DD)
(as 'date "2024.01.15")          ; 2024.01.15

; Parse time (HH:MM:SS.mmm)
(as 'time "12:30:00.000")        ; 12:30:00.000

; Parse timestamp (ISO 8601 input accepted; display uses the `D` separator)
(as 'timestamp "2024-01-15T12:30:00.000")  ; 2024.01.15D12:30:00.000000000

; Boolean to/from string
(as 'str true)           ; "true"
(as 'b8 "true")       ; true  (non-empty string → true)
(as 'b8 "")           ; false (empty string → false)
```

## 5. Null Semantics in Casting { #null-semantics }

Null values propagate through casts. A typed null in one type becomes a typed null in the target type. Invalid string parses raise a domain error.

```text
; Casting a typed null produces a typed null
(as 'f64 0Ni)           ; 0Nf  (null i32 → null f64)
(as 'i64 0Nf)           ; 0Nl  (null f64 → null i64)

; Null to string
(as 'str 0Ni)           ; null string

; Invalid string parse → domain error
(as 'i64 "not-a-number")  ; error: domain
(as 'date "bad-date")     ; error: domain
```

!!! note "Null literals by type"

    - `0Ni` — null i32
    - `0Nl` — null i64
    - `0Nh` — null i16
    - `0Nf` — null f64
    - `0Nd` — null date
    - `0Nt` — null time
    - `0Np` — null timestamp

    `SYM` has no `0N` literal (`0Ns` does not parse): its null is the empty
    symbol, written `'`. `(nil? ')` is `true`, and it prints as `'`, so a
    printed vector such as `[a ' b]` reads back as the same value. `STR`'s null
    is the empty string `""`.

    Use `nil?` to test for null values: `(nil? 0Ni)` returns `true`.

```lisp
; Null propagation through arithmetic
(+ 0Ni 10)              ; 0Nl  (null propagates; i32 + i64 → i64)
(* 0Nf 2.0)             ; 0Nf

; Null in vectors — sentinel null elements stay null.
; The null literal must match the vector's element type (0Nl for i64).
(+ [1 0Nl 3] 10)          ; [11 0Nl 13]
```

## 6. Symbol vs String { #sym-vs-str }

Rayforce has two string representations with different performance characteristics. Choosing the right one matters for large datasets.

| Feature | RAY_SYM (`'sym`) | RAY_STR (`'str`) |
|---|---|---|
| Storage | Dictionary-encoded (integer indices into global intern table) | Variable-length (inline for 12 bytes or fewer, pooled for longer) |
| Comparison | Integer comparison (fast) | Byte comparison with 4-byte prefix rejection |
| Best for | Categorical data with repeated values (status, country, product) | Unique or freeform text (names, descriptions, URLs) |
| Group-by speed | Very fast (integer hashing) | Slower (string hashing) |
| Memory | Low for high-repeat columns (one copy per unique value) | Proportional to total text length |

```lisp
; Creating symbol and string values
(set s 'Active)         ; symbol literal
(set t "hello world")   ; string literal

; Converting between them
(as 'sym "Active")     ; 'Active
(as 'str 'Active)      ; "Active"

; Symbol vectors (common in tables)
(set statuses ['Active 'Inactive 'Active 'Pending])
(type statuses)          ; 'SYM  (uppercase = vector)
```

!!! note "Rule of thumb"

    Use `'sym` for columns where you expect many repeated values (status codes, categories, tickers). Use `'str` for freeform text (descriptions, addresses, log messages).

## 7. Date/Time Types { #date-time }

Rayforce has three temporal types, all stored as integers internally:

| Type | Internal | Epoch | Literal Format |
|---|---|---|---|
| `date` | i32 | Days since 2000-01-01 | `2024.01.15` |
| `time` | i32 | Milliseconds since midnight | `12:30:00.000` |
| `timestamp` | i64 | Nanoseconds since 2000-01-01 midnight | `2024.01.15D12:30:00.000000000` |

**Date arithmetic:**

```lisp
; Add days to a date
(+ 2024.01.15 1)        ; 2024.01.16
(+ 2024.01.15 30)       ; 2024.02.14

; Subtract dates to get days between
(- 2024.01.15 2024.01.01)  ; 14

; Create a date range
(+ 2024.01.01 (til 7))   ; [2024.01.01 2024.01.02 ... 2024.01.07]
```

**Casting between temporal types:**

```lisp
; Date to timestamp (midnight)
(as 'timestamp 2024.01.15)  ; 2024.01.15D00:00:00.000000000

; Timestamp to date (strips time) — note the `D` separator in the literal
(as 'date 2024.01.15D12:30:00.000)  ; 2024.01.15

; Date to integer (days since epoch)
(as 'i64 2024.01.15)      ; 8780
```

## 8. GUID Type { #guid }

GUIDs are 16-byte unique identifiers displayed as hex strings. They are useful for primary keys, correlation IDs, and distributed systems.

`guid` takes a **count** and returns a GUID *vector* of that length:

```lisp
; Generate N random GUIDs
(guid 0)                  ; []                      (empty vector)
(guid 1)                  ; [daa76c35-...-dfb2fcab]  (one random GUID; random each time)
(guid 3)                  ; three random GUIDs

; GUIDs support equality comparison
(set g (guid 1))
(== g g)                  ; [true]

; Type check
(type (guid 1))           ; 'GUID  (uppercase = vector)
```

## 9. `sym-name` Function { #sym-name }

The `sym-name` function resolves integer symbol IDs into symbols. Given a symbol atom, it returns the symbol unchanged. Given an i64 (a raw sym table index), it returns the corresponding symbol.

```lisp
(sym-name 'hello)        ; 'hello  (passthrough)
(sym-name 0)              ; resolves sym ID 0 to its symbol

; Useful for building dynamic strings from symbols
;; To get a string from a symbol, use cast:
(as 'str 'hello)          ; "hello"
```

## 10. Practical Examples { #practical-examples }

### CSV Type Fixing

After loading a CSV, columns often arrive as strings. Cast them to proper types:

```lisp
; Load CSV (all columns as strings by default)
(write "/tmp/rayforce-types-trades.csv"
  "sym,price,qty,date\nAAPL,150.5,100,2024.03.01\nGOOG,2800.0,50,2024.03.10\n")
(set raw (.csv.read [STR STR STR STR] "/tmp/rayforce-types-trades.csv"))

; Cast columns to proper types
(set trades (table [price qty date sym]
  (list (as 'f64 (at raw 'price))
        (as 'i64 (at raw 'qty))
        (as 'date (at raw 'date))
        (as 'sym (at raw 'sym)))))
```

### Date Arithmetic for Analytics

```lisp
; Find records in the last 30 days
(set today 2024.03.15)
(set cutoff (- today 30))

(select {from: trades where: (>= date cutoff)})
```

### Building Typed Vectors

```lisp
; Build a date vector from strings
(set dates (as 'date ["2024.01.01" "2024.02.01" "2024.03.01"]))
; [2024.01.01 2024.02.01 2024.03.01]

; Build a symbol vector from strings
(set categories (as 'sym ["A" "B" "A" "C"]))
; ['A 'B 'A 'C]
```

### Type-Safe Table Construction

```lisp
; Construct a well-typed table
(set orders (table
  [id product price date]
  (list
    [1 2 3]
    ['Widget 'Gadget 'Widget]
    [9.99 24.50 9.99]
    (+ 2024.01.01 [0 1 2]))))

; Inspect the table shape. On a table, `meta` reports only the kind and the
; column count — it does not return per-column types.
(meta orders)
; {type:TABLE len:4}   (len = number of columns)

; To see a single column's element type, use `type`:
(type (at orders 'price))   ; 'F64
```

## Next Steps

- [**Data Types**](../data-types/index.md) — Full reference for all Rayforce types
- [**Math Operations**](../language/math.md) — Arithmetic, aggregation, and rounding functions
- [**String Operations**](../language/string.md) — upper, lower, trim, substr, replace, concat, like
- [**Select Queries**](../queries/select.md) — Filtering, grouping, and projecting with select
