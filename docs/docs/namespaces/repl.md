# `.repl.*` — interactive REPL control

Attach the local interactive REPL to a remote Rayforce server. While attached, every input line you type is sent over IPC (with `RAY_IPC_FLAG_VERBOSE` set so server-side stdout/stderr come back inline) and the remote result is printed. The prompt prefix gains the remote address so it's always visible which session you're driving.

`.repl.connect` is a thin wrapper over [`.ipc.open`](ipc.md#ipc-open) — it reuses the same host:port[:user:password] address syntax and the same error mapping — plus it stashes the resulting handle so the REPL's eval path picks it up automatically. Reach for it from inside an interactive session; it has no useful effect in a piped/script context (no term to flip the prompt on).

!!! note "Restricted under `-U`"
    Both builtins are `RAY_FN_RESTRICTED`. A remote-attached session is functionally identical to issuing the queries via `.ipc.send`, so the same blocking rules apply.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.repl.connect`](#repl-connect) | unary | restricted | Open an IPC session and route REPL input to it |
| [`.repl.disconnect`](#repl-disconnect) | variadic | restricted | Detach and close the active remote session |
| [`.repl.complete`](#repl-complete) | unary | — | Sorted names (builtins, keywords, session bindings) that start with a prefix |
| [`.repl.signature`](#repl-signature) | unary | — | How a named global is called: kind, calling convention, special form, lambda parameters, documented usage; calls nothing |
| [`.repl.definition`](#repl-definition) | unary | restricted | Where the function a named global holds was written: file, `(fn …)` span, text, and a digest of the loaded source |
| [`.repl.inspect`](#repl-inspect) | variadic | — | Kind, type and length of a named global or member path, with its dict member or table column names and types, read from headers only |

## `.repl.connect` { #repl-connect }

Signature: `(.repl.connect "host:port")` or `(.repl.connect "host:port:user:password")`.

Returns the IPC handle (same value `.ipc.open` would have returned) on success. Errors propagate from `.ipc.open` — see [`.ipc.open`](ipc.md#ipc-open) for the full error matrix.

If a prior remote session was already attached on a different handle, it's closed first to avoid leaking server-side connection slots. After a successful attach:

- The active REPL handle is set to the new value.
- The address (truncated to fit) is stored as the prompt prefix.
- Subsequent REPL inputs are evaluated remotely until `.repl.disconnect` is called.

```text
(.repl.connect "127.0.0.1:5000")
;; Prompt becomes:   127.0.0.1:5000>

;; Every line is now evaluated remotely:
(+ 1 2)        ;; => 3   (on the server)
(count trades) ;; => 1000000

(.repl.disconnect)
;; Prompt returns to local.
```

Errors do **not** automatically disconnect — most Rayfall errors (type, domain, parse, etc.) are recoverable; you stay attached and can keep typing. Call `.repl.disconnect` explicitly to leave.

## `.repl.disconnect` { #repl-disconnect }

Signature: `(.repl.disconnect)`. Closes the remote handle, clears the active-handle slot, restores the prompt prefix. Returns null. Idempotent — calling it without an active session is a no-op.

```lisp
(.repl.disconnect)
```

## `.repl.complete` { #repl-complete }

Signature: `(.repl.complete prefix)`. Returns a sorted `sym` vector of the names that begin with the string `prefix`: builtins, keywords and the bindings of the live session. It is the engine side of the REPL's own tab completion, exposed as a read-only call so editor tooling can ask a running server over IPC. History-word completion is not included, since it is local to the terminal client. A prefix that is not a string raises `type`.

```lisp
(.repl.complete "xas")      ;; => [xasc]
(.repl.complete "identi")   ;; => [identical]
```

## Editor metadata

`.repl.complete`, `.repl.signature`, `.repl.definition` and `.repl.inspect` answer an editor's questions about the live session over IPC. They share these rules:
- **Naming.** They take an explicitly named global, a symbol. It is the global of that exact name, dotted builtin names (`.sys.mem`) included. For a dotted name no global holds, the name is followed through dictionary members (`'D.f` is the member `f` of the dict `D`), never through a table column, a call or a method. Only `.repl.inspect`'s symbol-vector path may end at a table column.
- **Read-only.** They evaluate and call nothing.
- **Missing.** A missing binding is an ordinary result, not an error.
- **Restricted connections.** `.repl.complete`, `.repl.signature` and `.repl.inspect` are allowed under `-U`. They call and evaluate nothing, and give a restricted client nothing it cannot already get. Evaluating a name over IPC returns its value, a lambda, dict or table included. Printing a lambda gives only `lambda`, so that is not where the parameter names come from. `.repl.definition` is refused there with `access`, because it exposes server file paths and loaded source.

## `.repl.signature` { #repl-signature }

Signature: `(.repl.signature name)`, `name` a symbol. It returns a dict describing how the value the name holds is called, without calling it:

| Key | Value |
|---|---|
| `name` | the symbol asked for |
| `bound` | whether the name holds a value |
| `callable` | whether that value is a function |
| `kind` | `lambda`; `unary`, `binary` or `vary` for a builtin (its calling convention); `namespace` for a namespace root such as `.sys`; `value` for another bound value that is no function; `literal` for `true` and `false`; `missing` |
| `special` | whether it is a special form, which receives its arguments unevaluated (`if`, `select`, `set`, …) |
| `params` | a lambda's parameter names, a `sym` vector; empty otherwise |
| `arity` | a lambda's parameter count; `0Nl` for a builtin |
| `usage` | a builtin's documented call forms, the text its errors cite; `""` when it has none |
| `summary` | a builtin's one-line summary of what it does, the text the [function reference](../reference/all-functions.md) shows; `""` for anything else (a lambda, a value, a namespace root, a literal, a missing name): never invented |
| `builtin` | the builtin's own name, read from the function object: also when reached through an alias, and the held builtin's name when a builtin name was rebound to another builtin; the empty symbol otherwise |
| `restricted` | whether the builtin is refused under `-U` |
| `atomic` | whether the builtin is registered atomic: the evaluator maps it over vectors **and lists** itself, every argument (`+`, `neg`, `abs`, …; `(+ (list 1 2.5) 10)` is `(list 11 12.5)`); `false` for anything else |
| `elementwise` | a `B8` vector, one flag per argument: whether that argument maps a **vector** element by element, one result per element, from the engine's row-shape record (the one queries plan by: for `if` it describes a query's evaluation, see below). `[true]` for `sin`, `[true false]` for `in` (its set is taken whole), `[true false false]` for `substr`. Empty for a variadic builtin the record does not describe, and for anything that is not a builtin |
| `aggregate` | whether the builtin is registered as an aggregation, one that reduces a vector (`sum`, `count`, `avg`, `min`, …); `false` for anything else |

The three describe a builtin as the engine records it, not as a guess from its name:

- **`atomic` and `elementwise` differ.** An atomic builtin is element-wise in every argument. Many others map a vector element by element too, without being registered atomic: `sin`, `not`, `nil?`, `upper`, `like`, the date parts. They may refuse a list: `(upper (list "a" 'b))` is a `type` error.
- **A lookup argument is not element-wise.** An argument a builtin reads as a lookup is `false`, even where it maps: `at`'s index, `find`'s needles, `bin`'s values.
- **One form only, for builtins with more than one arity.** `elementwise` describes one form, and says nothing of the other:
  - `min` and `max` are described with one argument, as aggregates (`[false]`), although `(min a b)` maps element-wise;
  - `and` and `or` are described in their two-operand form (`[true true]`), and map their operands element by element in and out of a query: `(and [true false true] [true true false])` is `[true false false]`.
  - `if` is described in its two-operand form too (`[true true]`), but that is how a **query** evaluates it: there, `(if c 7)` over a column `c` gives one value per row (`[7 0 7]` for `c` = `[true false true]`). An ordinary call with a boolean-vector condition needs an else arm: `(if [true false true] 7)` is a `domain` error ("a boolean-vector condition needs an else arm"), while `(if [true false true] 7 9)` is `[7 9 7]`.
- **`first`, `last`, `top` and `bot` are aggregations too.** In a query they reduce a column, or each group, to one value (`top` and `bot`: the `k` largest or smallest), and are `aggregate: true`. Outside a query `first` and `last` take an element by position, as their own pages say.

A builtin's calling convention is not the set of forms it accepts. `min` is unary as a function value, but `(min 1 2)` takes two numbers. So `arity` is left unknown for a builtin, and `usage` spells its forms out. The answer follows the name's current value: an alias reports the function it holds, and a reassignment reports the new one. A name that is not a symbol raises `type`.

```lisp
(set add (fn [a b] (+ a b)))
(.repl.signature 'add)
;; {name:add bound:true callable:true kind:lambda special:false params:[a b] arity:2 usage:"" summary:"" builtin:' restricted:false atomic:false elementwise:[] aggregate:false}
(at (.repl.signature 'min) 'usage)
;; "(min <x: vector, atom or list>) or (min <a: num> <b: num>)"
(at (.repl.signature 'if) 'special)     ;; true
(at (.repl.signature 'nosuch) 'kind)    ;; 'missing
```

At the REPL, [`:h NAME`](../language/repl.md#help-for-a-name) prints the same facts for a person.

## `.repl.definition` { #repl-definition }

Signature: `(.repl.definition name)`, `name` a symbol. It returns a dict saying where the function the name holds now was written. That is not the assignment that bound the name: after `(set alias original)`, `alias` reports `original`'s source. It reads the source record a lambda keeps from parsing, the same record error traces use, so the parse and call paths do nothing extra for it.

| Key | Value |
|---|---|
| `name` | the symbol asked for |
| `bound` | whether the name holds a value |
| `kind` | `lambda`, `builtin`, `value` or `missing` |
| `available` | whether a source location is known |
| `file` | the file the lambda was loaded from; `"repl"` for input that came from no file (the REPL, an evaluated string) |
| `start`, `end` | `[line col]` of the `(fn …)` form: 0-based lines, 0-based byte columns, `end` exclusive |
| `text` | that form's source text |
| `digest`, `size` | the CRC-32 (8 hex digits) and the byte length of the whole text loaded with it, so an editor can tell its buffer has changed since |

Nothing is invented. When no location is known, `available` is `false`, `file`, `text` and `digest` are empty, `start` and `end` are empty vectors and `size` is `0Nl`. That covers:

- a builtin, a value or a missing name;
- a lambda built at run time from quoted pieces, as in `(eval (list 'fn ['x] 'x))`: its parts were written somewhere, but no `(fn` form was;
- a position past the parser's 16-bit line or column.

A name that is not a symbol raises `type`.

```lisp
;; lib.rfl holds:
;; (set add (fn [a b]
;;   (+ a b)))
(load "lib.rfl")
(.repl.definition 'add)
;; {name:add bound:true kind:lambda available:true file:"lib.rfl" start:[0 9] end:[1 10]
;;  text:"(fn [a b]\n  (+ a b))" digest:"974e76eb" size:31}
(at (.repl.definition 'min) 'available)   ;; false
```

## `.repl.inspect` { #repl-inspect }

Signature: `(.repl.inspect path)` or `(.repl.inspect path opts)`.

- `path` is a symbol, or a symbol vector `[root member …]`. The vector steps through dictionary members and may end at one table column: `['cfg 'db]` is the member `db` of the dict `cfg`, and `['t 'price]` is the column `price` of the table `t`. A dotted symbol (`'cfg.db`) follows dictionary members only, as for `.repl.signature`.
- `opts` is a dict with optional `prefix` (a string) and `limit` (a non-negative integer of any width, default 1000). An empty dict `{}` means no options.

It returns a dict:

| Key | Value |
|---|---|
| `path` | the path asked for |
| `bound` | whether the path names a value |
| `type` | the type `type` would give, for a stored column the type of the column it reads as (`I64`, `STR`, `DATE`, …); the empty symbol when missing |
| `kind` | `dict`, `table`, `vector`, `atom`, `list`, `lambda`, `builtin`, `other` or `missing` |
| `len` | members, rows or elements when held in a header; `0Nl` for an atom, a function, a missing path, or a count that would need a read |
| `keys` | the dict's member names or the table's column names that start with `prefix`, sorted, at most `limit` of them |
| `types` | the matching members' or columns' types, in the same order |
| `truncated` | whether `limit` left matching names out |

A symbol read from a stored `SYM` column (a dict keyed by one, or a path taken from one) is resolved through that column's symbol file, as printing it is, so its names are the ones `key` shows.

It reads headers only. It never reads a row, decodes a compressed column, or calls anything. A compressed column's type and row count come from its file header. A partitioned table's row count comes from its partition column. A plain partitioned column has no row count of its own, so its `len` is `0Nl`. The work is one pass over the members and a sort of the matching names, and at most `limit` names are returned.

Every outcome is distinct:
- A missing name, member or column gives `bound:false` and `kind:missing`; so does a path that steps into something that is not a dict, or past a column.
- An empty dict or table is `bound:true` with `len:0` and no keys.
- A dict keyed by anything other than symbols reports its `len`, with `keys` and `types` empty.

A path that is not a symbol or a non-empty symbol vector raises `type`. Options other than a dict, a `prefix` that is not a string, or a `limit` that is not an integer or is null raise `type`. A negative `limit` or an unknown option raises `domain`.

```lisp
(set cfg {db: {host: "h" port: 5000} tag: 'x})
(at (.repl.inspect 'cfg) 'keys)             ;; [db tag]
(.repl.inspect ['cfg 'db 'port])
;; {path:[cfg db port] bound:true type:i64 kind:atom len:0Nl keys:[] types:[] truncated:false}
(set t (table [x y s] (list [1 2 3] [1.0 2.0 3.0] ['a 'b 'c])))
(.repl.inspect 't)
;; {path:t bound:true type:TABLE kind:table len:3 keys:[s x y] types:[SYM I64 F64] truncated:false}
(.repl.inspect 't {prefix: "s" limit: 10})  ;; keys:[s] types:[SYM]
(at (.repl.inspect ['t 'y]) 'type)          ;; 'F64
```

## See also

- [`.ipc.*`](ipc.md) — programmatic alternative when you want to drive a remote server from inside a script or another Rayfall expression.
- [REPL Reference](../language/repl.md) — top-level REPL features, line editor, history.
