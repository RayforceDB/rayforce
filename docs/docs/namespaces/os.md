# `.os.*` — process environment

Process-environment access. The namespace is deliberately small: two builtins read and write environment variables. Filesystem metadata lives in its own [`.fs.*`](fs.md) namespace.

!!! note "Restricted under `-U`"
    `.os.getenv` and `.os.setenv` are `RAY_FN_RESTRICTED` (environment is process-global state).

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.os.getenv`](#os-getenv) | unary | restricted | Read an environment variable as a string. |
| [`.os.setenv`](#os-setenv) | binary | restricted | Set an environment variable. |

## `.os.getenv` { #os-getenv }

Signature: `(.os.getenv "NAME")`. Returns the value as a string, or the empty string if the variable is unset.

Errors: `type` (arg not a string), `domain` (empty arg).

```lisp
(.os.getenv "HOME")
;; => "/Users/anton"

(.os.getenv "NONEXISTENT")
;; => ""
```

## `.os.setenv` { #os-setenv }

Signature: `(.os.setenv "NAME" "value")`. Both args must be strings. Returns `value` (retained) so the call composes inside larger expressions.

Errors: `type` (either arg not a string), `domain` (null pointer).

```lisp
(.os.setenv "RAY_DEBUG" "1")
```

There is no unset: setting a variable to the empty string (or to `"0"`) is
how a script turns a switch off again.  Rayforce's on/off switches
(`RAY_IDX_STATS`, the `RAY_CSV_TRACE` trace) are on
only for a value other than the empty string and `0`, in this process and in
the processes it starts, which inherit the variable.

```lisp
(.os.setenv "RAY_CSV_TRACE" "1")   ;; trace the next imports
(.os.setenv "RAY_CSV_TRACE" "")    ;; and off again
```

## See also

- [`.fs.*`](fs.md) — filesystem metadata (`size` / `list`).
- [`.sys.exec`](sys.md#sys-exec) — shell out for anything not covered here.
