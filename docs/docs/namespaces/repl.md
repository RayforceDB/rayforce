# `.repl.*` — interactive REPL control

Attach the local interactive REPL to a remote Rayforce server. While attached, every input line you type is sent over IPC (with `RAY_IPC_FLAG_VERBOSE` set so server-side stdout/stderr come back inline) and the remote result is printed. The prompt prefix gains the remote address so it's always visible which session you're driving.

`.repl.connect` is a thin wrapper over [`.ipc.open`](ipc.md#ipc-open) — it reuses the same host:port[:user:password] address syntax and the same error mapping — plus it stashes the resulting handle so the REPL's eval path picks it up automatically. Reach for it from inside an interactive session; it has no useful effect in a piped/script context (no term to flip the prompt on).

!!! note "Restricted under `-U`"
    Both builtins are `RAY_FN_RESTRICTED`. A remote-attached session is functionally identical to issuing the queries via `.ipc.send`, so the same blocking rules apply.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.repl.connect`](#repl-connect) | unary | restricted | Open an IPC session and route REPL input to it. |
| [`.repl.disconnect`](#repl-disconnect) | variadic | restricted | Detach and close the active remote session. |
| [`.repl.complete`](#repl-complete) | unary | — | Names the engine knows that start with a prefix, sorted. |
| [`.repl.signature`](#repl-signature) | unary | — | How a named global is called: kind, convention, parameters, usage. |

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

`.repl.complete` and `.repl.signature` answer an editor's questions about the live session over IPC. They share these rules:
- **Naming.** They take an explicitly named global, a symbol. It is the global of that exact name, dotted builtin names (`.sys.mem`) included. For a dotted name no global holds, the name is followed through dictionary members (`'D.f` is the member `f` of the dict `D`), never through a table column, a call or a method.
- **Read-only.** They evaluate and call nothing.
- **Missing.** A missing binding is an ordinary result, not an error.
- **Restricted connections.** Both are allowed under `-U`. They call and evaluate nothing, and give a restricted client nothing it cannot already get. Evaluating a name over IPC returns its value, a lambda included, with its parameters. Printing a lambda gives only `lambda`, so that is not where the parameter names come from.

## `.repl.signature` { #repl-signature }

Signature: `(.repl.signature name)`, `name` a symbol. It returns a dict describing how the value the name holds is called, without calling it:

| Key | Value |
|---|---|
| `name` | the symbol asked for |
| `bound` | whether the name holds a value |
| `callable` | whether that value is a function |
| `kind` | `lambda`; `unary`, `binary` or `vary` for a builtin (its calling convention); `value` for a bound value that is no function; `missing` |
| `special` | whether it is a special form, which receives its arguments unevaluated (`if`, `select`, `set`, …) |
| `params` | a lambda's parameter names, a `sym` vector; empty otherwise |
| `arity` | a lambda's parameter count; `0Nl` for a builtin |
| `usage` | a builtin's documented call forms, the text its errors cite; `""` when it has none |
| `builtin` | the builtin's own name, read from the function object: also when reached through an alias, and the held builtin's name when a builtin name was rebound to another builtin; the empty symbol otherwise |
| `restricted` | whether the builtin is refused under `-U` |

A builtin's calling convention is not the set of forms it accepts. `min` is unary as a function value, but `(min 1 2)` takes two numbers. So `arity` is left unknown for a builtin, and `usage` spells its forms out. The answer follows the name's current value: an alias reports the function it holds, and a reassignment reports the new one. A name that is not a symbol raises `type`.

```lisp
(set add (fn [a b] (+ a b)))
(.repl.signature 'add)
;; {name:add bound:true callable:true kind:lambda special:false params:[a b] arity:2 usage:"" builtin:' restricted:false}
(at (.repl.signature 'min) 'usage)
;; "(min <x: vector, atom or list>) or (min <a: num> <b: num>)"
(at (.repl.signature 'if) 'special)     ;; true
(at (.repl.signature 'nosuch) 'kind)    ;; 'missing
```

## See also

- [`.ipc.*`](ipc.md) — programmatic alternative when you want to drive a remote server from inside a script or another Rayfall expression.
- [REPL Reference](../language/repl.md) — top-level REPL features, line editor, history.
