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

## See also

- [`.ipc.*`](ipc.md) — programmatic alternative when you want to drive a remote server from inside a script or another Rayfall expression.
- [REPL Reference](../language/repl.md) — top-level REPL features, line editor, history.
