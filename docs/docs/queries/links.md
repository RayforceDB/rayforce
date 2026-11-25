# Linked Columns

An integer column whose values are row indices into another table.  Field access through the link compiles to a single array indirection per row — no hash probe, no comparison.

## Why Linked Columns

The standard way to relate two tables is a foreign-key column: each row holds a key value (a sym, a string) that *matches* the primary key of another table.  Resolving such a key at query time requires a hash lookup against the target's keyed column — one hash + comparison per row.

A **linked column** stores *row indices* instead of key values.  At construction time you pre-compute, for each row, the position of its match in the target table.  Resolution at query time becomes a single `target[link[i]]` array access — an O(1) indirection with no hashing.

This is the same trade made by enumerated columns in column-store databases that support them: lose the flexibility of carrying user-visible keys, gain O(1) deref on the hot path.

## Linking a Column in a Table

The realistic shape: you have two tables and want a foreign-key-style relationship between them.  Build a row-id column on the dependent side, attach the link, and stash it back into the table.  All subsequent queries through that table automatically dereference through the link.

### Signature

`(.col.link target int-vec)`

Attach the column as a link to the table named by `target` (a sym).  Validates that `target` resolves to a `RAY_TABLE` in the current environment and that the input is a numeric int column.  Returns a new column with `HAS_LINK` set; the underlying integer values are unchanged.

### Worked Example: orders → customers

Two tables: `customers` with `id`, `name`, `city`; `orders` with `oid`, `qty`, and a `cust` column holding row indices into `customers`.

```lisp
; Set up the target ('customers' must be bound in env before linking).
(set customers (table [id name city]
    (list [100 200 300]
          (list 'alice 'bob 'carol)
          (list "NYC" "LA" "SF"))))

; Build orders with a 'cust' column that holds row indices into customers.
; Each row says "this order belongs to customers[cust]".
(set orders (table [oid qty cust]
    (list [10 11 12 13]
          [5  2  7  3]
          [0  2  1  0])))

; Tag the cust column as a link.  In-place update via the dict form;
; from: 'orders means "modify the table bound to this symbol", and the
; right-hand side `cust` resolves to the existing column inside update.
(update {from: 'orders cust: (.col.link 'customers cust)})
```

`orders.cust` still reads as a column of integers `[0 2 1 0]` — the link is invisible to anything that doesn't ask for it.  But field-walks across the link now work transparently.

## Querying Through the Link

Dotted-field access on a linked column resolves through the link automatically.  No special syntax: `orders.cust.name` walks the existing dotted-name resolver, produces a column the same length as `orders.cust`, with values gathered from `customers.name` at the row indices.

```lisp
orders.cust.name       ; ⇒ [alice carol bob alice]
orders.cust.city       ; ⇒ ["NYC" "SF" "LA" "NYC"]
orders.cust.id         ; ⇒ [100 300 200 100]
```

You can pre-compute the dereferenced values and pipe them through the rest of the query:

```lisp
; Pre-resolve customer names per order, then use as a regular column.
(set names orders.cust.name)
(count names)              ; ⇒ 4
(distinct names)           ; ⇒ [alice carol bob]
```

Each dotted-field deref allocates one fresh column and gathers values once.  The cost is one allocation + *n* indirect reads, regardless of target field type — no hash probe, no string comparison.  Compared to expressing the same query as an `inner-join` on the customer ID, the linked-column form skips the hash-table build entirely.

!!! note "Dotted access inside `select` / `update` clauses is not yet supported."
    Using `cust.name` directly in a projection (`(select {from: orders n: cust.name})`), a `by:` clause, or a `where:` predicate currently errors out at compile time.  Today's workflow is to resolve the linked column at the top level — `(set names orders.cust.name)` — and pass the bound name into the query: `(select {from: orders n: names})` works, as does `(select {from: orders qty: (sum qty) by: names})`.  Wiring dotted-deref through the query compiler is a follow-up.

### Null Propagation

Two sources of nulls in the result:

- If `linked[i]` is itself null (the row-id column contains a null sentinel at row `i`), the result row is null.
- If `linked[i]` is in range but the target's row at that index is null, the result row is null.
- Out-of-range row IDs (`rid < 0` or `rid >= target.len`) silently become null in the result.  This matches the prior column-store convention — safer than erroring out mid-query.

### Bind to a Local Then Aggregate

To run aggregations against a derived field, resolve the dotted access at the top level, bind the result, and pass it to ordinary collection / aggregate operators:

```lisp
(set cities orders.cust.city)   ; ⇒ ["NYC" "SF" "LA" "NYC"]
(distinct cities)              ; ⇒ ["NYC" "SF" "LA"]
(count cities)                 ; ⇒ 4
```

The group-by-via-`select` form (`(select {from: orders qty: (sum qty) by: cust.city})`) is not yet supported — the select compiler doesn't currently recognise dotted-path column expressions inside `where` / `by` / projection clauses.  Until that lands, materialise through a temp.

## Introspection

| Function | Returns | Notes |
|---|---|---|
| `(.col.link? v)` | boolean | True iff the column has `HAS_LINK` set. |
| `(.col.target v)` | sym or null | The target table sym, or null when no link is attached. |
| `(.col.unlink v)` | v with link removed | Drops `HAS_LINK`.  Underlying int values are preserved. |

## Storage and Persistence

A linked column is internally a regular `RAY_I32` or `RAY_I64` vector with the `RAY_ATTR_HAS_LINK = 0x04` attribute bit set.  The target table sym ID is stored at bytes 8–15 of the column's nullmap union.  The link is preserved across in-place mutation, and the bit is orthogonal to `RAY_ATTR_HAS_INDEX` — a column can simultaneously carry a link and a hash / sort / zone / bloom [accelerator index](https://github.com/RayforceDB/rayforce/blob/master/src/ops/idxop.h).

Slices of a linked column inherit the link transparently.  The slice's own attribute byte does not carry `HAS_LINK` — that bit lives only on the parent — but `(.col.link? slice)`, `(.col.target slice)`, and dotted-field access on the slice all read through to the parent's link.  Both the row-id values and the link target are sourced from the parent at deref time.

On disk, linked columns serialize via `ray_col_save` into a normal column file plus a `.link` sidecar that holds the target sym name in plain text.  Sym IDs are process-local, so the sidecar is the only durable identity for the link.  Both `ray_col_load` (buddy-copy) and `ray_col_mmap` (zero-copy / splayed-table mmap mode) re-attach the link on load.  Saving an unlinked column removes any stale sidecar.

## Parted-Table Interaction

Linked columns work with [parted (partitioned) tables](../storage/index.md) in one direction only: a parted **fact** table may carry a linked column that points at a *non-parted* dim table (in-memory or splayed). The reverse — a link whose target is itself parted — is rejected with `error: nyi: target table has a parted column`. Today's deref math (`target_col[linkcol[i]]`) is straight indexing with no notion of which segment a global rowid lives in; pointing a link at a parted target would need a partition-locator pass that v1 doesn't provide.

The check fires at **two points**: at attach time (`(.col.link 'parted_dim ...)` errors immediately) and at deref time (when the linked sym was bound to a non-parted table at attach but later rebound to a parted one, or when a link reattached from a `.link` sidecar resolves to a parted target). Without the deref-time check, lazy rebinds and disk-loaded links would slip past the attach guard and produce a silent wrong-answer bug.

### Supported shape

- Many parted segments (e.g. `2024.01.01/facts/`, `2024.01.02/facts/`) all hold their own copy of the link sidecar pointing to the same dim sym name.
- `ray_read_splayed` (used internally by `ray_read_parted`) re-attaches `HAS_LINK` on every per-segment column via `try_load_link_sidecar`; the per-VM-segment vectors that come out the other end carry `HAS_LINK` + `link_target` exactly as a non-parted load would.
- Segment streaming (`build_segment_table`) extracts `segs[seg_idx]` by retain, so each per-segment view in the streamed flat table inherits the link without any extra plumbing.  Deref against the (unchanging, non-parted) dim is the standard `ray_link_deref` path.

### Why not parted dims?

Two design forks would need to be settled before parted-target links could ship: (1) global rowids (link payload offsets into the conceptual concatenation of all dim partitions, requiring an O(log p) partition-locator search per deref), or (2) per-partition rowids (the link is local to its own partition; the dim must be partitioned the same way as the fact). Both have legitimate use cases — the design is deferred until a real workload picks one. For now, keep dim tables unpartitioned (a small splayed directory or in-memory table fits all common workloads), or fall back to a regular hash join inside the streamed query.

## Caveats and Limits

- **Integer types only.**  `RAY_I32` and `RAY_I64` are the only valid carriers.  Attaching to `RAY_BOOL`, `RAY_STR`, or any float/temporal type returns a type error — those types either don't have enough range to address realistic tables or alias the bytes 8–15 nullmap slot for their own metadata (`str_pool`, `sym_dict`).
- **Lazy resolution.**  The target sym is looked up against the global environment at deref time.  If you rebind the target table (e.g. `(set customers ...)`) the link automatically follows.  If the target sym is rebound to something that's not a table, the dotted-walk surfaces `error: name: 'orders.cust.field' undefined` — the link does not silently no-op.
- **No bounds check at attach time.**  `(.col.link)` trusts that every row id fits in the target's row count.  Out-of-range values surface as null in the deref result, not at construction.
- **One-hop deref only (v1).**  Single-step deref works through any number of dotted segments AS LONG AS only one of them crosses a link — `orders.cust.name` works because `cust` is the only linked segment.  Chains of multiple linked columns — `orders.cust.addr_id.street` when both `cust` and `addr_id` are linked — do not work today: `ray_link_deref` returns a plain int column without propagating `HAS_LINK`, so subsequent dotted segments error out as *undefined*.  Fused chain-deref is planned.
- **Non-parted dim only.**  Link targets must be regular (non-parted) tables; attaching to a target that has any `RAY_PARTED` column fails with `error: nyi: link: target table has a parted column`.  Parted facts can still carry links — just point them at non-parted dim tables.  See [Parted-Table Interaction](#parted-table-interaction) above.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `.col.link` | `(.col.link target int-vec)` | Attach a foreign-key link from an int vector to a named table |
| `.col.unlink` | `(.col.unlink v)` | Detach the link metadata, returning a plain int vector |
| `.col.link?` | `(.col.link? v)` | Test whether a value carries link metadata |
| `.col.target` | `(.col.target v)` | Read back the link's target table name (or `null`) |
| `linked.field` | dotted access | Dereference the link, gathering target.field at each row |
