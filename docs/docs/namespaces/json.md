# `.json.*` — JSON text

Reads JSON text into values and writes values out as JSON. The reader is strict ([RFC 8259](https://www.rfc-editor.org/rfc/rfc8259)). It never evaluates anything and never interns anything, so it is safe on text that arrived from outside. Malformed text is an error that names the byte; you never get a partial value back.

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.json.parse`](#json-parse) | variadic | — | JSON text to a value; element-wise over a string vector. |
| [`.json.get`](#json-get) | variadic | — | One field of each document, by a dotted path, without building the rest. |
| [`.json.format`](#json-format) | unary | — | A value to JSON text. |

All three are pure and always available, including in [restricted mode](../storage/ipc.md).

## `.json.parse` { #json-parse }

Signature: `(.json.parse s)` or `(.json.parse s limits)`.

`s` is a string. A string vector, or a list of strings, is read element by element: the result has one value per element and collapses like an array (below). The empty string is the string null, and it reads as `null`.

| JSON | Value |
|---|---|
| object | dict with `STR` keys and `LIST` values |
| array of one kind of scalar | that vector: `I64`, `F64`, `STR` or `B8` |
| any other array | `LIST` |
| integer in `I64` range | `i64` |
| any other number | `f64` |
| `true` / `false` | `b8` |
| `null` | `null` |
| string | `str` |

```lisp
(set d (.json.parse "{\"kind\":\"note\",\"ids\":[1,2,3],\"ok\":true}"))
(at d "kind")                 ; "note"
(at d "ids")                  ; [1 2 3]
(.json.parse "[1,2.5,null]")  ; [1.0 2.5 0Nf]
(.json.parse "[1,\"a\"]")     ; (list 1 "a")
(.json.parse ["1" "" "3"])    ; [1 0Nl 3]
```

**Object keys are strings.** Look a field up with `(at d "kind")` or `(get d "kind")`. Keys are not interned as symbols. Interning keeps every distinct name for the life of the process, and input from outside would make that unbounded. If a key appears twice, the dict keeps the key's first position and its last value: `{"a":1,"b":2,"a":3}` reads as `(dict ["a" "b"] (list 3 2))`.

**Arrays.** An array becomes a typed vector when every element is the same kind of scalar, in the same way `map` collapses its results:

- strings and nulls give `STR`, with a null as `""` (the string null);
- integers and nulls give `I64`, with a null as `0Nl`;
- numbers and nulls where some number is not an integer give `F64`, with each integer converted (an integer past 2^53 loses precision: `[9007199254740993,0.5]` reads as `[9007199254740992.0 0.5]`);
- booleans give `B8`, but only with no null, since `B8` has no null.

Every other array is a `LIST`: one with mixed kinds, a nested array or object, or only nulls. `[]` is an empty `LIST` and `{}` an empty dict. An empty string vector reads as an empty `LIST`, as an array of no elements (or of only nulls) does.

**Numbers.** A number written without a fraction or an exponent, and within the `I64` range, is an `i64`. Every other number is an `f64`: `1.0`, `1e3`, a number beyond the `I64` range, and `-9223372036854775808`, which is the `I64` null `0Nl`. A number beyond the `F64` range (`1e999`) is a `domain` error, since the `F64` domain has no infinity. `-0` is the integer `0`. JSON's number grammar is enforced: a leading zero (`01`), a bare `.5` or `1.`, a leading `+`, `NaN` and `Infinity` are `parse` errors.

**Strings.** Every JSON escape is decoded: `\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t` and `\uXXXX`. A surrogate pair decodes to its one code point. The following are `parse` errors:

- a lone surrogate (a high one not followed by a low one, or a low one on its own);
- any other escape;
- a raw control character (below U+0020);
- bytes that are not valid UTF-8.

A byte order mark before the value is not JSON whitespace, and is a `parse` error.

**Errors.** The error code is `parse` for malformed text, `domain` for an unrepresentable number, `limit` for a cap and `type` for an argument that is not a string. The message names the byte offset, and for a vector input the element:

```lisp
(.json.parse "{\"a\":[1,2,}")
; error: parse: .json.parse: unexpected character '}', expected a value at byte 10
(.json.parse ["[1]" "[1"])
; error: parse: .json.parse: unexpected end of input, expected ',' or ']' at byte 2 of element 1
```

**Limits.** The caps are those of [`read-value`](../language/read-value.md#limits), and an optional `limits` dict overrides them by the same names. Four apply to JSON:

| Limit | Applies to |
|---|---|
| `input` | bytes of one document |
| `depth` | nesting depth; the default is 128, and it never exceeds 1024 |
| `nodes` | values, containers included |
| `alloc` | bytes of the blocks the reader allocates for the values it builds: atoms, vectors with their string pools, lists with their growth, dicts with their keys and values, and the decoded-string buffer; an array of one kind of scalar is built straight into its typed vector, so it costs that vector (an array of 780,000 integers, 8 MB), and the atoms an array turns into when it turns out mixed are charged while the typed run still is, each string atom with its bytes, then the run's charge is returned. The real peak can still reach twice the cap, because a vector grows by doubling: the block a vector grows into is up to twice what it holds (measured under a 4 MB cap: 1.5 times the cap for an array of integers, twice it for a string followed by nulls) |

The other two names, `grammar` and `symbol-bytes`, are accepted but have no effect on JSON. A cap is checked before the value it counts is built (a container's own growth is settled when it closes), so a hostile document stops at the cap and never exhausts the stack or the heap: 100,000 empty objects hold about 25 MB, and `{alloc: 8000000}` refuses them. For a vector, the caps apply to each element. A long document can be interrupted: the reader polls for cancellation every 1,024 members of a container.

Object keys are checked for repeats by sorting them, so the cost of an object is O(n log n) in its keys whatever they are; no choice of keys can make it slower.

```lisp
(.json.parse "[[[1]]]" {depth: 2})
; error: limit: .json.parse: depth limit of 2 exceeded at byte 2
```

## `.json.get` { #json-get }

Signature: `(.json.get s path type)`, `(.json.get s path type limits)`, or untyped `(.json.get s path)`, `(.json.get s path limits)`.

Returns the value at `path` in the document `s`, or in each document of a string vector. Only that value is built. The rest of the document is still checked, so a malformed document is an error here too.

### With a type: the form to use in queries

`type` is `'STR`, `'I64` or `'F64`. The result is a vector of that type with one cell per document (for one document, an atom of that type), whatever the documents hold, so a query over it cannot fail because one row is odd. A field that is missing, is JSON `null`, or holds a value of another kind gives that type's null (`""`, `0Nl`, `0Nf`):

| `type` | takes | gives null for |
|---|---|---|
| `'STR` | JSON strings | numbers (a number is not turned into text), booleans, objects, arrays, `null`, a missing field |
| `'I64` | numbers written as integers within the `I64` range | fractions and exponents, numbers past the range, everything else |
| `'F64` | every JSON number (an integer converted, losing precision past 2^53) | everything else |

There is no boolean type: `B8` has no null, and reading a missing field as `false` would make it pass a `where:` that tests for `false`. `'B8` is a `domain` error; use the untyped form and compare the field with `true`. Another name is a `domain` error naming the accepted ones.

A null (empty) document gives the type's null, like a missing field. `-0` is a number written as an integer, so `'I64` reads it as `0` and `'F64` as `0.0`. A document that is not JSON is still an error naming the element and the byte, never a silent null: a stored line that does not parse is a fault to see, not a field to skip.

```lisp
(set t (table [line] (list ["{\"kind\":\"note\",\"n\":2}" "{\"kind\":5}" "{}" ""])))
(select {from: t where: (== (.json.get line "kind" 'STR) "note")})   ; the first row
(at (select {from: t n: (+ 1 (.json.get line "n" 'I64))}) 'n)        ; [3 0Nl 0Nl 0Nl]
```

The type, the path and the limits may each be held by a name; in a `select` the typed call takes the same one-call-over-the-column route as the untyped one, in every position.

A quoted type in the call is a type even when the table has a column of that name: in a table with a column `STR`, `(.json.get line "k" 'STR)` still reads strings. A type passed through a wrapper lambda's parameter, `(g line "k" 'STR)` with `(set g (fn [p q r] (.json.get p q r)))`, follows the rule for any lambda argument in a query: a quoted symbol that names a column is that column.

### Untyped

`path` is a string (or a symbol) of keys separated by dots. A key made of digits, `"0"` or `"12"` but not `"01"`, also indexes an array: `"data.ids.0"` is the first element of the `ids` array of the `data` object. A missing path gives `null`, not an error. That covers a missing key, an index past the end, a key into a scalar, and a null (empty) document. An empty path, or one with an empty segment (`"a..b"`), is a `domain` error. A key that contains a dot cannot be addressed this way; use `.json.parse` for it. If a key is repeated, the last one is used, as `.json.parse` keeps it.

Over a string vector, `path` may also be a string or symbol vector of the same length, one path per element; a different length is a `length` error, and a null path (`""`) gives `null` for its element. One document with a vector of paths gives one value per path, typed as a column is.

`limits` is the dict `.json.parse` takes. Only the value at the path is charged against `nodes` and `alloc`; the rest of the document is charged its `input` and `depth` only, so a document of a million values can still give one field.

```lisp
(set doc "{\"kind\":\"note\",\"data\":{\"ids\":[10,20,30]}}")
(.json.get doc "kind")         ; "note"
(.json.get doc "data.ids.1")   ; 20
(.json.get doc "data.ids")     ; [10 20 30]
(.json.get doc "missing")      ; null
```

Over a string vector, the untyped result has one value per element. It is typed from every element's value, with a missing path counting as null:

| The values found | Result |
|---|---|
| strings | `STR`, with a missing path as `""` |
| integers | `I64`, with a missing path as `0Nl` |
| numbers, some of them not integers | `F64`, with a missing path as `0Nf` |
| booleans, none missing | `B8` |
| none: the path is missing everywhere, or there are no elements | `STR` of `""` (an empty `STR` for no elements) |
| anything else | `LIST`, with a missing path as `null` |

A string field therefore gives a `STR` column, and `==` against a string is false on the rows that lack it. Two consequences of typing by the values found:

- A field absent from every row the call sees gives a `STR` of nulls, whatever the field holds elsewhere: under a `where:` that keeps only rows without `n`, `(+ 1 (.json.get line "n"))` meets a string and is a `type` error.
- One row whose field is another kind makes the whole column a `LIST`: with one row `{"kind":1}` among strings, `(== (.json.get line "kind") "note")` is a `type` error for the whole query, not `false` for that row.


```lisp
(set t (table [line] (list ["{\"kind\":\"note\",\"n\":1}" "{\"kind\":\"ping\",\"n\":2.5}" "{\"n\":3}"])))
(select {from: t where: (== (.json.get line "kind") "note")})   ; the first row
(at (select {from: t n: (.json.get line "n")}) 'n)              ; [1.0 2.5 3.0]
```

In a `select`, a `.json.get` or `.json.parse` of a column is called once over the column, also when it stands inside element-wise builtins such as `(+ 1 (.json.get line "n"))` or `(== (.json.get line "k") "x")`. A projection, a `where:`, a `by:` and an `update` therefore type the result in the same way. Integers of a field that also holds non-integers become `F64` and, past 2^53, lose precision, as in an array. A path or a limits dict may be written in place or held by a name (a global, a lambda's parameter, a `let`): `(.json.get line P)` with `(set P "n")` is still one call over the column. A call whose text is a constant is one value, like any constant: `(select {from: t x: (.json.get "{\"a\":1}" "a")})` gives `1` on every row. Inside a lambda the call runs per row, as every builtin does there, and a missing field is then an untyped `null`. They work on any string column: in memory, splayed, compressed (generation 2) and parted.

## `.json.format` { #json-format }

Signature: `(.json.format v)`. Returns a string of JSON text.

| Value | JSON |
|---|---|
| dict with symbol or string keys | object |
| dict with other scalar keys (a keyed table made by `xkey` on one column) | object whose member names are the keys as `.json.format` writes them, as strings: a number its JSON text (`1` is `"1"`, the F64 `1.0` is `"1.0"`), a date, time, timestamp, timespan or guid its printed form (`"2024.01.01"`), a null key `""`. Two keys written alike give two members of one name, which most readers resolve to the last: `(.json.format (dict ["" ""] (list 1 2)))` writes `{"":1,"":2}`, and two null keys of a keyed table both write `""`. A multi-column key is a `type` error |
| table, in memory, splayed, compressed or parted | array of objects, one per row (a parted table's partition key is a column) |
| list, vector | array |
| `i16` `i32` `i64` `u8` | number |
| `f32` `f64` | the shortest text that reads back to the same float: the fewest significant digits that do, written as the shorter of the positional and the exponent form (the positional on a tie, so a value from 1e17 to 1e21 can be written with more digits, `73950701333918976.0`, when that is shorter), always with a `.` or an exponent: `1.0`, `0.1`, `120.0`, `1e2`, `99999999999999984.0`, `1e300`, `5e-324`; the same text whatever the host's `LC_NUMERIC` is |
| `b8` | `true` / `false` |
| string, symbol | string; the null string and the null symbol are `""` |
| date, time, timestamp, timespan, guid | the string it prints as (`"2024.01.01"`) |
| `null` and the null of a typed value | `null` |

A string is escaped as JSON requires: `"` and `\` are escaped, `\b \f \n \r \t` use their short escapes, and other control characters become `\u00XX`. Every other character is written as its UTF-8 bytes. The following are errors:

- a string that is not valid UTF-8 (`domain`);
- a dict whose keys are not scalars, such as a keyed table on several columns (`type`);
- a lambda or another value JSON cannot hold (`type`);
- nesting deeper than 1024 (`limit`).

The evaluator refuses a bare `null` argument for every builtin, so `(.json.format null)` is a `type` error, while a `null` inside a container is written as `null`.

```lisp
(.json.format (dict ["a" "b"] (list 1 [1.5 2.0])))   ; "{\"a\":1,\"b\":[1.5,2.0]}"
(.json.format (table [a b] (list [1 2] ["x" "y"])))  ; "[{\"a\":1,\"b\":\"x\"},{\"a\":2,\"b\":\"y\"}]"
(.json.format [1 0Nl 3])                             ; "[1,null,3]"
```

### Round trips

`(.json.parse (.json.format v))` gives back `v` in these cases:

- dicts with string keys;
- `I64`, `F64`, `STR` and `B8` vectors;
- numbers, booleans and strings;
- lists of those, with the exceptions below.

It does not give back `v` in these cases:

- symbol keys and values come back as strings;
- temporal values come back as strings;
- a table comes back as a list of dicts;
- a list of one kind of scalar, nulls included, comes back as that vector (`(list null 1)` gives `[0Nl 1]`);
- a null string comes back as `""`, which is the string null.
