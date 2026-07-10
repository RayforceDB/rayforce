# Column Attributes

Semantic properties stamped onto columns — `sorted`, `unique`, `grouped`, `parted`.  Where an [accelerator index](indexes.md) is a *physical* structure (a hash table, a permutation), a column attribute is an *assertion about meaning*: this column is non-descending, these values are distinct.  The `.attr.*` family layers over the same storage as `.idx.*`, but its contract is semantic, not structural.

!!! note "v1 status"
    The four attributes, their strict verify-on-set kernels, and the `(.attr.*)` Rayfall surface are shipped. `sorted` and `unique` are numeric; the backing-index attributes `grouped` and `parted` also support symbol columns.

## Why Attributes Are Separate from Indexes

An [accelerator index](indexes.md) answers *"what structure is attached?"* — a zonemap, a hash table, a sort permutation.  A column attribute answers a different question: *"what is semantically true of these values?"*  The two markers (`sorted`, `unique`) carry no allocation at all — they are cheap flags.  The two backing-index attributes (`grouped`, `parted`) do reuse the index layer underneath (so a `grouped` or `parted` column also shows up via `.idx.has?` and `.idx.info`), but `.attr.*` exists to assert a *property* the engine can trust, not merely to build a structure.

The payoff is that consumers can trust a stamped attribute unconditionally. Besides the [as-of join executor](joins.md#pre-sorted-fast-path), filters and grouped aggregation use the physical layout directly.

## The Four Attributes

| Attribute | Category | Asserts | Backing |
|---|---|---|---|
| `sorted` | marker | Values are physically non-descending | None — a cheap flag, no allocation |
| `unique` | marker | All values are distinct | None — a cheap flag, no allocation |
| `grouped` | backing index | value → row-id groups are available | Reuses the hash index |
| `parted` | backing index | Column is laid out as contiguous, ascending value-blocks | Builds a part index (value → `[start, len)` range) |

The two **markers** are independent and may coexist.  A column holds at most **one backing index** at a time: setting `grouped` or `parted` replaces any prior backing index.  Markers may coexist with a backing index.

`parted` is **verify-only** — it never reorders data.  It checks that the column is already block-laid-out (each distinct value occupies one contiguous run; numeric keys must also be ascending) and errors if it is not.  The caller is responsible for ordering the column first.

For `where: (in key set)` on a parted key, the filter builds its row selection from the matching `[start, len)` ranges without scanning the key column or expanding full morsels into row ids. If the same query groups by that key and uses supported aggregates, Rayforce skips the filter and group discovery entirely and streams each selected partition directly into the aggregate kernel. The equivalent `grouped` path consumes hash-index row slices.

## Setting, Reading, and Dropping

### Signatures

`(.attr.set 'name v)   (.attr.get v)   (.attr.drop v)`

- `(.attr.set 'name v)` — assert attribute `name` on vector `v`. Strict verification **errors on violation**, so a stamped attribute never lies. Returns the column with the attribute recorded.
- `(.attr.get v)` — return the symbol vector of currently-held attributes (the empty symbol vector when none are set).
- `(.attr.drop v)` — remove all attributes from `v`.

### Examples

```lisp
(.attr.get (.attr.set 'sorted [1 2 2 5 9]))                       ; ⇒ ['sorted]
(.attr.get (.attr.set 'unique [1 2 3 4]))                         ; ⇒ ['unique]
(.attr.get (.attr.set 'grouped [1 1 2 3 3]))                      ; ⇒ ['grouped]
(.attr.get (.attr.set 'parted [1 1 1 2 2 3]))                     ; ⇒ ['parted]

; No attributes set — the empty symbol vector.
(.attr.get [1 2 3])

; Drop clears whatever was set.
(.attr.get (.attr.drop (.attr.set 'sorted [1 2 3])))             ; ⇒ no attributes
```

A `sorted` marker works on any numeric column, including temporal types:

```lisp
(.attr.get (.attr.set 'sorted [10:00:01.000 10:00:02.000 10:00:03.000])) ; ⇒ ['sorted]
```

A `parted` column carries a real part index, inspectable through the shared `.idx.*` surface — its `kind` is `part` and `n_parts` is the number of distinct value-blocks:

```lisp
(at (.idx.info (.attr.set 'parted [1 1 2 2])) 'kind)             ; ⇒ 'part
(at (.idx.info (.attr.set 'parted [1 1 2 2])) 'n_parts)          ; ⇒ 2
(.idx.has? (.attr.set 'grouped [1 1 2 3 3]))                      ; ⇒ true
```

Float blocks work too, as long as the layout holds:

```lisp
(.attr.get (.attr.set 'parted [1.0 1.0 2.0]))                    ; ⇒ ['parted]
```

## Strict Verify on Set

`.attr.set` is a *checked assertion*, not a hint.  It scans the column and errors if the property does not hold — so any consumer downstream can trust the stamp without re-verifying.

```text
(.attr.set 'sorted [3 1 2])     ; ⇒ error (domain): not non-descending
(.attr.set 'unique [1 2 2 4])   ; ⇒ error (domain): duplicate
(.attr.set 'parted [1 2 1 2])   ; ⇒ error (domain): not block-laid-out
```

An unknown attribute name is rejected the same way:

```text
(.attr.set 'bogus [1 2 3])      ; ⇒ error (domain): unknown attribute
```

Setting `sorted` runs an O(n) ascending scan; `unique` and `grouped` build / consult the hash to detect duplicates; `parted` walks the column verifying that each distinct value forms one contiguous block (and ascending block order for numeric keys).

## Combining Attributes

The two markers are independent and stack:

```lisp
; markers coexist
(.attr.get (.attr.set 'unique (.attr.set 'sorted [1 2 3])))       ; ⇒ ['sorted 'unique]
```

A marker may also coexist with a backing index:

```lisp
(.attr.get (.attr.set 'unique (.attr.set 'grouped [1 2 3 4])))    ; ⇒ ['unique 'grouped]
```

But a column holds at most one backing index — setting `grouped` or `parted` replaces whichever was there before.

## Conservative Propagation

Attributes propagate **conservatively**.  Only operations that trivially preserve the values — a refcount / copy-on-write copy, a plain rebind — keep the attributes.  *Any* transform that could change ordering, length, or values (arithmetic, `filter` / `where`, `reverse`, `concat`, reordering `take`) **drops** them.  This keeps the invariant honest: a held attribute always reflects the current bytes.

```lisp
; Arithmetic drops the marker (and a backing index).
(.attr.get (+ (.attr.set 'sorted [1 2 3]) 1))                    ; ⇒ no attributes
(.idx.has? (+ (.attr.set 'grouped [1 2 3]) 1))                   ; ⇒ false

; Reorder / concat drop the marker.
(.attr.get (reverse (.attr.set 'sorted [1 2 3])))               ; ⇒ no attributes
(.attr.get (concat (.attr.set 'sorted [1 2 3]) [0 1]))          ; ⇒ no attributes

; Plain rebind preserves.
(set _s (.attr.set 'sorted [1 2 3]))
(.attr.get _s)                                                   ; ⇒ ['sorted]
```

**Re-assert after a transform.**  If you sort, filter, or otherwise rebuild a column and still want the property recognized, call `.attr.set` again on the result.

## The As-of Join Fast Path

The biggest payoff today is in `asof-join` (see [Joins](joins.md#pre-sorted-fast-path)).  The as-of executor would otherwise sort both inputs by `(equality-keys, time)` on every call.  Attributes let it skip that sort when the data already satisfies the required ordering.  These are pure **opt-in acceleration** — when an attribute is present the executor skips a sort; when it is absent it falls back to the usual sort-merge, so results are identical either way.

### Un-partitioned: a lone time key

Equality keys are now **optional**.  A single time key runs an un-partitioned as-of join.  If both time columns carry `sorted`, both sorts are skipped (O(n+m) instead of O(n log n)):

```lisp
(set Lp (table [Time Price] (list [10:00:01.000 10:00:03.000] [100.0 101.0])))
(set Rp (table [Time Bid]   (list [10:00:00.000 10:00:02.000 10:00:04.000] [99.0 100.5 101.5])))
(asof-join [Time] Lp Rp)

; Stamp the marker in place when building the table:
(set Ls (table [Time Price] (list (.attr.set 'sorted [10:00:01.000 10:00:03.000]) [100.0 101.0])))
(.attr.get (at Ls 'Time))                                        ; ⇒ ['sorted]
```

### Partitioned: a `parted` numeric key

For a partitioned join `(asof-join [Key Time] L R)`, if the single numeric equality key is `parted` and its time column is non-descending within each part, that side's sort is skipped.  Note `parted` is **numeric-only** — the equality key here is a numeric ID, not a symbol:

```lisp
(set Lgp (table [ID Time Price]
    (list (.attr.set 'parted [1 1 2 2])
          [10:00:01.000 10:00:03.000 10:00:01.000 10:00:02.000]
          [1.0 2.0 3.0 4.0])))
(set Rgp (table [ID Time Bid]
    (list (.attr.set 'parted [1 1 2 2])
          [10:00:00.000 10:00:02.000 10:00:00.000 10:00:01.000]
          [0.9 1.8 2.9 3.8])))
(asof-join [ID Time] Lgp Rgp)
```

## Caveats and Limits

- **Type support.** `sorted` and `unique` accept numeric types. `grouped` and `parted` also accept `SYM`; `STR` remains unsupported.
- **Strict verify, no silent stamping.**  `.attr.set` errors on violation rather than recording a property it cannot confirm; this is what lets consumers trust the stamp unconditionally.
- **Conservative propagation.**  Only copy / rebind preserve attributes; every transform drops them.  Re-assert with `.attr.set` after a transform.
- **One backing index per column.**  `grouped` and `parted` share the [accelerator-index](indexes.md) slot, so a column carries at most one of them at a time; the markers are separate and free.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `.attr.set` | `(.attr.set 'name v)` | Verify and stamp `sorted` / `unique` / `grouped` / `parted`; errors on violation |
| `.attr.get` | `(.attr.get v)` | Symbol vector of held attributes (empty when none) |
| `.attr.drop` | `(.attr.drop v)` | Remove all attributes from `v` |
