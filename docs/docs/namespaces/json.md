# `.json.*` — JSON text

Reads JSON text into values and writes values out as JSON. The reader is strict ([RFC 8259](https://www.rfc-editor.org/rfc/rfc8259)). It never evaluates anything and never interns anything, so it is safe on text that arrived from outside. Malformed text is an error that names the byte; you never get a partial value back.

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.json.parse`](#json-parse) | variadic | — | JSON text to a value; element-wise over a string vector. |
| [`.json.get`](#json-get) | binary | — | One field of each document, by a dotted path, without building the rest. |
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
- numbers and nulls where some number is not an integer give `F64`, with each integer converted;
- booleans give `B8`, but only with no null, since `B8` has no null.

Every other array is a `LIST`: one with mixed kinds, a nested array or object, or only nulls. `[]` is an empty `LIST` and `{}` an empty dict.

**Numbers.** A number written without a fraction or an exponent, and within the `I64` range, is an `i64`. Every other number is an `f64`: `1.0`, `1e3`, a number beyond the `I64` range, and `-9223372036854775808`, which is the `I64` null `0Nl`. A number beyond the `F64` range (`1e999`) is a `domain` error, since the `F64` domain has no infinity. JSON's number grammar is enforced: a leading zero (`01`), a bare `.5` or `1.`, a leading `+`, `NaN` and `Infinity` are `parse` errors.

**Strings.** Every JSON escape is decoded: `\"`, `\\`, `\/`, `\b`, `\f`, `\n`, `\r`, `\t` and `\uXXXX`. A surrogate pair decodes to its one code point. The following are `parse` errors:

- a lone surrogate (a high one not followed by a low one, or a low one on its own);
- any other escape;
- a raw control character (below U+0020);
- bytes that are not valid UTF-8.

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
| `alloc` | bytes of the values built |

The other two names, `grammar` and `symbol-bytes`, are accepted but have no effect on JSON. A cap is checked before the value it counts is built, so a hostile document stops at the cap and never exhausts the stack or the heap. For a vector, the caps apply to each element.

```lisp
(.json.parse "[[[1]]]" {depth: 2})
; error: limit: .json.parse: depth limit of 2 exceeded at byte 2
```

## `.json.get` { #json-get }

Signature: `(.json.get s path)`.

Returns the value at `path` in the document `s`, or in each document of a string vector. Only that value is built. The rest of the document is still checked, so a malformed document is an error here too.

`path` is a string (or a symbol) of keys separated by dots. A key made of digits, `"0"` or `"12"` but not `"01"`, also indexes an array: `"data.ids.0"` is the first element of the `ids` array of the `data` object. A missing path gives `null`, not an error. That covers a missing key, an index past the end, a key into a scalar, and a null (empty) document. An empty path, or one with an empty segment (`"a..b"`), is a `domain` error. A key that contains a dot cannot be addressed this way; use `.json.parse` for it. If a key is repeated, the last one is used, as `.json.parse` keeps it.

```lisp
(set doc "{\"kind\":\"note\",\"data\":{\"ids\":[10,20,30]}}")
(.json.get doc "kind")         ; "note"
(.json.get doc "data.ids.1")   ; 20
(.json.get doc "data.ids")     ; [10 20 30]
(.json.get doc "missing")      ; null
```

Over a string vector, the result has one value per element. It is typed from every element's value, with a missing path counting as null:

| The values found | Result |
|---|---|
| strings | `STR`, with a missing path as `""` |
| integers | `I64`, with a missing path as `0Nl` |
| numbers, some of them not integers | `F64`, with a missing path as `0Nf` |
| booleans, none missing | `B8` |
| none: the path is missing everywhere | `STR` of `""` |
| anything else | `LIST`, with a missing path as `null` |

A string field therefore gives a `STR` column, and `==` against a string is false on the rows that lack it:

```lisp
(set t (table [line] (list ["{\"kind\":\"note\",\"n\":1}" "{\"kind\":\"ping\",\"n\":2.5}" "{\"n\":3}"])))
(select {from: t where: (== (.json.get line "kind") "note")})   ; the first row
(at (select {from: t n: (.json.get line "n")}) 'n)              ; [1.0 2.5 3.0]
```

In a `select`, `.json.get` and `.json.parse` are called once over the column, so a projection, a `where:`, a `by:` and an `update` all type the result in the same way. They work on any string column: in memory, splayed, compressed (generation 2) and parted.

## `.json.format` { #json-format }

Signature: `(.json.format v)`. Returns a string of JSON text.

| Value | JSON |
|---|---|
| dict with symbol or string keys | object |
| table | array of objects, one per row |
| list, vector | array |
| `i16` `i32` `i64` `u8` | number |
| `f32` `f64` | the shortest number that reads back to the same float, always with a `.` or an exponent (`1.0`, `0.1`, `1e+300`) |
| `b8` | `true` / `false` |
| string, symbol | string |
| date, time, timestamp, timespan, guid | the string it prints as (`"2024.01.01"`) |
| `null` and the null of a typed value | `null` |

A string is escaped as JSON requires: `"` and `\` are escaped, `\b \f \n \r \t` use their short escapes, and other control characters become `\u00XX`. Every other character is written as its UTF-8 bytes. The following are errors:

- a string that is not valid UTF-8 (`domain`);
- a dict whose keys are not symbols or strings (`type`);
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
