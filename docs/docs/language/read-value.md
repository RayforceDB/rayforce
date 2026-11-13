# Reading Data Text

`read-value` turns text into one value **without evaluating anything**. It reads
what [`repr`](string.md#repr) writes, plus a few literal forms that are
convenient to write by hand, so a file of Rayfall text can hold configuration or
saved state and be loaded safely.

`(eval (parse s))` is not a safe way to load data: it runs whatever the text
says, including `(.sys.exec ...)` and `(write ...)`. `read-value` has a closed
grammar, takes no names from the environment, and has fixed resource limits.

```lisp
ray> (read-value "{lookback: 20 symbols: ['BTCUSDT 'ETHUSDT] fee: 0.0004}")
{lookback:20 symbols:[BTCUSDT ETHUSDT] fee:4.00e-04}

ray> (type (read-value (repr (table [a b] (list [1 2] ["x" "y"])))))
'TABLE

ray> (read-value "(.sys.exec \"rm -rf x\")")
error: parse: read-value: only list, dict, table and as can be called here at byte 1
```

## Signatures

| Form | Result |
|---|---|
| `(read-value text)` | the value, with the default limits |
| `(read-value text limits)` | the value, with the limits named in the dictionary `limits` |
| `(read-limits)` | the default limits, as a dictionary |

The text must hold exactly one value. Whitespace and `;` comments around it are
allowed; a second value or any other trailing text is an error.

## Grammar

| Form | Value |
|---|---|
| `true` `false` | `B8` |
| `0x07` | `U8` |
| `2h` `3i` `4` | `I16` `I32` `I64` |
| `1.5e` `1.5` | `F32` `F64`; `-0.0` keeps its sign |
| `2024.01.02` `12:34:56.789` `2024.01.02D01:02:03.000000004` `0D01:00:00` `0Wn` | `DATE` `TIME` `TIMESTAMP` `TIMESPAN` |
| `0Nl` `0Ni` `0Nh` `0Nf` `0Ne` `0Nd` `0Nt` `0Np` `0Nn` | the typed null of that type |
| `"text"` | `STR`; escapes `\n \t \r \\ \"` and `\ooo` (octal) |
| `'name` | `SYM` atom; the name uses `A-Z a-z 0-9 _ . -`, and `'` alone is the empty symbol |
| `null` | the generic null |
| `[1 2 3]` `[1h 2h]` `["a" "b"]` `['a 'b]` | a typed vector; its elements are literals of one type |
| `(as 'T [])` | an empty vector of type `T` |
| `(as 'SYM "any text")` | a `SYM` atom whose name may contain anything |
| `(as 'SYM (list "a b" "c"))` | a `SYM` vector |
| `(as 'GUID "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0")` | a `GUID` atom |
| `(as 'GUID (list (as 'GUID "...") 0Ng))` | a `GUID` vector |
| `(list v ...)` and `()` | a `LIST` |
| `(dict keys values)` | a `DICT`; `keys` and `values` are vectors or lists of equal length |
| `(table names columns)` | a `TABLE`; `names` is a symbol vector and `columns` a list of vectors or lists |
| `{k: v ...}` | a `DICT` with name or string keys and a `LIST` of values |

Type tags are `B8 U8 I16 I32 I64 F32 F64 DATE TIME TIMESTAMP TIMESPAN GUID SYM STR`.

Things worth knowing:

- An empty vector always needs its type: `[]` is an error, `(as 'I64 [])` is not.
- Vector literals do not promote: `[1 2.5]` and `[1h 2i]` are errors.
- A bare name inside `[...]` is a symbol, as in the language (`[a b]` is
  `['a 'b]`). Outside brackets a bare name is an error.
- `list`, `dict`, `table` and `as` are keywords of this grammar. They are not
  looked up, so rebinding them in the environment changes nothing.
- A table keeps exactly the columns it is given. A zero-row table keeps its
  schema, including `LIST` columns, without inferring a column type from
  elements. A table with no columns has no rows.
- Dictionaries and tables keep their order and duplicate keys; nothing is sorted
  or coerced.
- Strings are byte sequences. `\000` is a NUL byte; bytes above 127 pass through.

## What is refused

| Input | Error |
|---|---|
| a name, any other call, a lambda, an assignment, `eval`, `(.sys.exec ...)` | `parse` |
| a second value, a missing closer, an unterminated string, an unknown escape | `parse` |
| a character literal such as `'a'`, an integer beyond `I64`, a float beyond its width | `parse` |
| a table whose names and columns differ in count, columns of different lengths, a dict with unequal key and value counts | `domain` |
| any [limit](#limits) exceeded | `limit`, naming the budget |

The error names the byte offset. Nothing is evaluated and nothing partial is
returned.

## Identity

For every value `repr` can write, `(read-value (repr v))` has the same type,
the same container kind, the same length, and the same stored values:

- integers of every width, without a trip through floating point, including
  values above 2^53 and every typed null;
- `F32` and `F64` at their own width, with exact finite values and the sign of
  zero. The engine's null model is unchanged, and infinities and NaN payloads
  are not values (an out-of-range float literal is an error);
- strings and symbols byte for byte, including NUL, quotes and delimiters. Symbol
  **ids** are not identity: the text is.
- `GUID`, temporal types to the nanosecond, the generic null;
- `LIST`, `DICT` and `TABLE` recursively: key and value kinds, order, empty
  shapes, zero-row typed and `LIST` columns, zero-column tables.

Not part of identity: reference counts, allocator addresses, cache and index
flags, and whether a `SYM` atom is marked as a quoted literal (the reader always
marks it, so the value is never taken as a name later).

Functions, builtins and error values have no data text: `repr` fails for them,
including when nested.

## Limits

Every limit is finite and inclusive: reaching it is allowed, exceeding it fails.
Override any of them with a dictionary: `(read-value s {nodes: 5000 alloc: 1048576})`.

| Name | Default | What it counts |
|---|---|---|
| `input` | 8 MiB | encoded bytes of the text, checked before anything is read |
| `depth` | 128 | nesting depth, the root value being depth 1. Never above 1024, whatever is asked |
| `nodes` | 1,000,000 | data occurrences: every scalar, vector element and container |
| `grammar` | 1,000,000 | syntax nodes: every token, call and type tag |
| `symbol-bytes` | 1 MiB | decoded bytes of every symbol occurrence, repeated names included |
| `alloc` | 64 MiB | peak live bytes the reader owns: the input copy, staging buffers, symbol staging and every value |

`alloc` is charged by the allocator's actual block size, headers included, and a
growing buffer is charged for the old and the new block together. It is **not**
a process memory limit: other heaps, thread stacks and concurrent work are
outside it. A block is admitted against the limit before it is allocated, so an
oversized input fails with a `limit` error rather than exhausting memory.

The limits bound one read. They do not bound how many symbols a runtime
accumulates across many successful reads; a host that needs that bound refuses
the symbols at the embedding boundary (the C API takes a total symbol ceiling,
`max_symbols`).

`repr` has the matching writer limit: `(repr v max-bytes)`, default 8 MiB,
fails with `limit` rather than returning truncated text. Equal byte limits on
both sides do **not** guarantee that every written value reads back under the
defaults: a small text can still need more depth, nodes or allocation than the
reader allows.

## Symbols and failed reads

Symbols are staged while the text is read and published once, only when the
whole value has been read and checked. A rejected read, whatever the reason,
adds no symbols. A successful read adds exactly the names that were new, in one
step under the symbol lock; other threads never see a partial set.

## Compatibility

No cross-version promise is made yet. Text written by one release is not
guaranteed to read in another, and an application that stores read-value text
should carry its own schema version.

The text is ordinary Rayfall, so `(eval (parse s))` reads it too. That is not a
substitute: only `read-value` refuses to run what the text says.
