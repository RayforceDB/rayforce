# `.fs.*` — filesystem

Minimal filesystem-metadata primitives. The namespace is deliberately small: two builtins cover the common needs (file size, directory listing) and the predicate cases (exists / is-file / is-dir) are reached by `try`-wrapping `.fs.size` or `.fs.list`.

!!! note "Always available"
    `.fs.size` and `.fs.list` are read-only and unrestricted — they remain callable for IPC peers even when the server is started with `-U`.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.fs.size`](#fs-size) | unary | — | File size in bytes |
| [`.fs.list`](#fs-list) | unary | — | Sorted sym vector of directory entries |

## `.fs.size` { #fs-size }

Signature: `(.fs.size "path")`. Returns the file size in bytes as an `i64`.

Errors: `type` (arg not a string), `io` (path missing or names a directory rather than a file). Wrap in `try` to distinguish "doesn't exist" from "is a directory" without parsing error messages — both surface as `io`.

```lisp
(.fs.size "/etc/hosts")
;; => 213

(try (.fs.size "/missing") 'missing)
;; => 'missing
```

## `.fs.list` { #fs-list }

Signature: `(.fs.list "path")`. Returns a sym vector of entries in the directory, sorted lexicographically. The `.` and `..` entries are filtered out.

Errors: `type` (arg not a string), `io` (path is not a directory, or doesn't exist).

```lisp
(.fs.list "/etc")
;; => ['hosts 'passwd 'resolv.conf ...]
```

Use this as a building block — `find`-style recursive walks can be expressed by mapping `.fs.list` over the result and filtering with `.fs.size` / `try`.

## See also

- [`.os.*`](os.md) — process-environment access (`getenv` / `setenv`).
- [`.sys.exec`](sys.md#sys-exec) — shell out for anything not covered here (test predicates, deletes, recursive operations).
