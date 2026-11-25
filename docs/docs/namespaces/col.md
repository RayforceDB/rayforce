# `.col.*` — column linking

A *linked column* is an integer vector whose values are row IDs into a named table. The link is metadata attached to the vector — its values are still plain integers — and the engine uses the link to resolve `linked.field` as a gather through the target table's column for `field`. This is the Rayfall analogue of a foreign-key reference: the link travels through `select`, joins, and accelerator indexes without copying any data. Reach for `.col.link` whenever you have an `int` column of row IDs (or a `where`-clause that produces one) and you want to slice the parent table by that selection.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.col.link`](#col-link) | binary | — | Attach a foreign-key link from an int vector to a named table |
| [`.col.unlink`](#col-unlink) | unary | — | Detach the link metadata, returning a plain int vector |
| [`.col.link?`](#col-link-p) | unary | — | Test whether a value carries link metadata |
| [`.col.target`](#col-target) | unary | — | Read back the link's target table name (or `null`) |
| [`fkeys`](#fkeys) | unary | — | Inspect linked-column metadata as `column -> target table` |

## `.col.link` { #col-link }

Signature: `(.col.link target rids)` where `target` is a symbol naming a global table and `rids` is a vector of integer (`I64`/`I32`/`I16`) row IDs.

Returns: a new vector that aliases `rids`'s data and is tagged with a link to `target`. Subsequent `linked.field` deref calls gather column `field` of the target table at the given row IDs.

Errors:

- `type` if `target` isn't a sym atom or `rids` isn't an int vector.
- `name` if `target` doesn't resolve to a table in the global environment.

The link is independent from any accelerator index on the same vector — you can stack `(.idx.zone (.col.link 'custs rids))` and both metadata survive together. See [`.idx.*`](idx.md).

Example:

```lisp
(set custs (table [id age name]
  (list [100 200 300] [18 25 42] (list 'alice 'bob 'carol))))

(set rids   [2 0 1 2])
(set linked (.col.link 'custs rids))

linked.id     ;; => [300 100 200 300]
linked.age    ;; => [42 18 25 42]
linked.name   ;; => (list 'carol 'alice 'bob 'carol)
```

## `.col.unlink` { #col-unlink }

Signature: `(.col.unlink v)`.

Returns: a vector with the same values as `v` but with the link metadata stripped. If `v` had no link, the call is an identity (returns `v` retained). Useful when you want to break the dotted-deref shortcut and treat the IDs as plain integers, e.g. for arithmetic.

Example:

```lisp
(set unlinked (.col.unlink linked))
(.col.link? unlinked)   ;; => false
unlinked                ;; => [2 0 1 2]
```

## `.col.link?` { #col-link-p }

Signature: `(.col.link? v)`. Returns a bool.

Cheap predicate; safe to call on anything (atom, vector, table — non-vectors always return `false`). The check is metadata-only and does not validate that the link's target table still exists.

```lisp
(.col.link? linked)   ;; => true
(.col.link? rids)     ;; => false
```

## `.col.target` { #col-target }

Signature: `(.col.target v)`. Returns the link's target table name as a sym atom, or the null object if `v` is not linked.

Slice-aware: if `v` is a slice into a linked parent, `.col.target` reads the parent's link, not the slice's offset.

```lisp
(.col.target linked)            ;; => 'custs
(.col.target rids)              ;; => null
(.col.target (take linked 2))   ;; => 'custs  (slice inherits)
```

## `fkeys` { #fkeys }

Signature: `(fkeys table)`.

Returns: a dictionary whose keys are linked column names and whose values are the linked target table names. Columns without link metadata are omitted, so a table with no linked columns returns `{}`.

```lisp
(set orders (table [cust amount]
  (list (.col.link 'custs [2 0 1])
        [12.5 30.0 7.0])))

(fkeys orders)   ;; => {cust:'custs}
```

## See also

- [Linked Columns](../queries/links.md) — query-level patterns, joins through links, multi-hop dereferencing.
- [Accelerator Indexes](../queries/indexes.md) — links coexist with `.idx.*` attachments.
