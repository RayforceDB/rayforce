# Indexes Guide

This guide is the procedural companion to [Indexes Overview](../queries/indexes-overview.md) (the map of what exists) and [Accelerator Indexes](../queries/indexes.md) (the per-function reference). Read those first if you haven't; this page assumes you know *what* the kinds are and focuses on *when* and *how*.

## 1. When to bother building an index { #when }

Rayforce's hot path already scans columnar data fast: each operator processes 1024-element morsels, and full-column work is cache-friendly and SIMD-amenable out of the box. An index pays off only when the build cost is amortized across many queries that exploit it.

Three rules of thumb to start with.

- **One-shot scans:** don't build an index. The build is itself a full pass; you'd pay it twice for a single query with no payoff.
- **Repeated point lookups against the same column:** a hash index lets filter EQ, filter IN, and find use O(matches) hash probes instead of O(n) scans.  Build it for any column that sees repeated equality queries during a session.
- **Range or sorted-output queries against a stable column:** a sort index gives the filter site an O(log n) binary-search path and lets ORDER BY (single ascending key) and distinct skip re-sorting entirely.

A useful lower bound: if you are not planning to run at least a handful of queries against the indexed column before mutating it, the index loses on round-trip cost.

## 2. Choosing an accelerator kind { #choose }

The four `.idx.*` kinds occupy the same per-column slot today — you pick one.

### Equality probes — `.idx.hash`

Suited to "does this value exist in the column" / "which row(s) hold this value" queries. Build cost O(n); space O(n). Handles duplicates via chained open-addressing — equally happy on a unique-key column or one with heavy repetition.

**Wired today:** filter EQ (integer-family, null-free columns), filter IN, find.  Float equality and null-bearing columns fall back to the scan.

### Range queries / sorted access — `.idx.sort`

Suited to "values between A and B", ordered-output queries, and distinct. Build cost O(n log n); space O(n) for the row-id permutation. The original column stays in original order — the sort lives in the permutation, not the values.

**Wired today:** filter range predicates EQ/LT/LE/GT/GE with a selectivity guard (span > n/128 → fallback), single-key ascending ORDER BY (descending falls back), distinct. NE predicates fall back.

### Whole-column pruning — `.idx.zone`

Suited to filter predicates that may fall outside the column's value range. Cheap to build, cheap to keep — effectively three numbers (min, max, null count) per column. Conceptually a column-level analogue of the optimizer's separate [partition-pruning pass](../architecture/pipeline.md); the two are not yet integrated.

**Wired today:** filter comparisons EQ/NE/LT/LE/GT/GE — O(1) all/none short-circuit for integer and float columns. Null-bearing columns fall back.

### Membership rejection — `.idx.bloom`

Suited to cheap "definitely not in this set" rejection, accepting some false positives that fall back to the real check. The 64-bit default is sized for small-to-mid columns; build cost O(n · k) with k = 3 hashes.

**Wired today:** filter EQ on integer-family columns — proves the key is absent and returns 0 rows without scanning. False positives fall through; null-bearing columns fall back.

## 3. Workflow: hot column, repeated lookups { #workflow-analytics }

The most common use of `.idx.hash`. You have a column that gets queried many times during a session, the values are stable for that session, and each probe today is a linear scan.

```lisp
; Build column.
(set v (* 7 (til 1000)))   ; 0, 7, 14, ..., 6993

; Attach the hash index once.  The executor will use hash probes from now on.
(set v (.idx.hash v))
(.idx.has? v)                  ; ⇒ true
(.idx.info v)                  ; ⇒ {kind:hash length:1000 ...}

; Membership probes use the hash fast path.
(in 700  v)                    ; ⇒ true   (700 = 7 × 100) — O(1) hash probe
(in 701  v)                    ; ⇒ false  — hash proves absent

; Point find uses the hash fast path.
(find v 700)                   ; ⇒ 100  — minimum row id via hash chain

; Explicit drop — only needed if you want to release the structure early.
; If you're about to mutate, you can skip this: the in-place mutators
; (insert 'v ...) / (alter 'v ...) drop the index automatically.
(set v (.idx.drop v))
(.idx.has? v)                  ; ⇒ false
```

`.idx.drop` is for *explicit release of the structure* — you call it when you want the per-index memory back before the column itself goes out of scope. You do **not** need to call it before a mutation (the in-place mutators `(insert 'v val)`, `(alter 'v set i val)`, and `(alter 'v concat vals)` all drop any attached index transparently as part of the write path) and you do **not** need it to switch to a different kind: calling another `.idx.*` attach on a column that already has one drops the existing kind first as part of the attach.

## 4. Workflow: ANN over embeddings { #workflow-ann }

HNSW is the vector-search index — built and consulted at query time via `(ann)`. For cosine-similarity nearest-neighbor over a list of float vectors:

```lisp
; A list of three 4-dimensional embeddings.
(set vecs (list
  [0.1 0.2 0.3 0.4]
  [0.9 0.8 0.7 0.6]
  [0.5 0.5 0.5 0.5]))

; Build the index.  metric ∈ {cosine, l2, ip}.
(set h (hnsw-build vecs 'cosine 16 200))

; Query: top-2 nearest neighbors of [0.1 0.2 0.3 0.4].
(ann h [0.1 0.2 0.3 0.4] 2 50)
; ⇒ table { _rowid: I64, _dist: F64 } sorted ascending by _dist

; Optional: release early if you want the memory back before scope exit.
; Otherwise, the handle auto-frees when refcounting drops it.
(hnsw-free h)
```

For one-off queries, brute-force `(knn vecs query k)` needs no index and is fine on small vector sets. Switch to HNSW once you have many queries against the same vector set.

See [Vector Search & HNSW](../graph/vector-search.md) for the full reference, including persistence (`hnsw-save` / `hnsw-load`) and metric trade-offs.

## 5. Workflow: cross-table reference via linked columns { #workflow-link }

A *linked column* stores row-id references into another table; dereferencing follows the link and pulls the target row at query time. Useful when you have a large fact column whose values are dictionary-style references into a smaller dimension table.

The full surface and worked examples live on the [Linked Columns](../queries/links.md) page. From an indexing perspective, the link *is* the cross-table index: there is no separate structure to build, query, or invalidate. With parted facts, the link points at a non-parted dim — per-segment `HAS_LINK` is preserved through `ray_read_parted` and segment streaming, so deref works inside streamed queries without extra plumbing. Parted-target dims are rejected at attach time; see [Linked Columns: Parted-Table Interaction](../queries/links.md#parted-table-interaction).

## 6. Workflow: partition pruning on parted tables { #workflow-parted }

For very large datasets, partition by the discriminator that filter predicates target most often:

- **Date partitioning** — the canonical choice for time-series. Filters of the form `where: (> Date 2024.01.01)` let the optimizer load only the matching partition directories.
- **Integer partitioning** — for bucketed numeric data (e.g. user-id ranges, region codes).
- **Symbol partitioning** — for categorical data with stable, low-cardinality discriminators.

The directory layout drives partition selection — see [Storage Guide: partitioned tables](storage.md#partitioned-tables) for the on-disk shape and [Block Offloading](../architecture/offloading.md) for how the optimizer streams across partitions without loading them all at once.

## 7. Lifecycle gotchas { #lifecycle }

Five things that bite first-time users.

- **Mutation drops the index.** The quoted-symbol forms `(insert 'v val)` and `(alter 'v set i val)` / `(alter 'v concat vals)` mutate `v` in place and invalidate the attached structure. The mutator paths handle the drop transparently — there's no error, just a silently un-indexed column afterward. Rebuild explicitly after the write. The non-quoted form `(insert v val)` returns a fresh value and leaves the original (still indexed) untouched.
- **Slices can't carry an index.** Internally, slicing a column produces a fresh slice header without `RAY_ATTR_HAS_INDEX`; `(.idx.*)` attach refuses to operate on a slice ("cannot index a slice; materialize first"). The Rayfall surface for slices today is C-API-only — this is mostly relevant when calling Rayforce from your own C code.
- **One slot per column, one kind at a time.** Calling `.idx.hash` on a column that already has `.idx.zone` drops the zone first. Multiple coexisting kinds per column is a v2 feature.
- **Persistence is explicit.** A splayed save (`.db.splayed.set`) persists a hash index built with `.idx.hash`, and writes each numeric column's chunk zone and each string column's dictionary; the CSV and Parquet converters add no hash index of their own. Other `.idx.*` indexes are not persisted: rebuild after a load. HNSW handles can be persisted explicitly with `hnsw-save` / `hnsw-load`.
- **Type support.** `.idx.hash` accepts numeric/boolean columns **and `RAY_SYM`** (it indexes the symbol domain ids). `.idx.zone`, `.idx.sort`, and `.idx.bloom` are numeric/boolean only. `RAY_STR` is deferred for all kinds, and `RAY_SYM` is deferred for the non-hash kinds — an unsupported column is rejected with `error: nyi: <kind>: only numeric/sym vectors supported (got type N)`, because the string/sym pool pointers collide with the index aux bytes and the displacement sweep is pending.

## 8. Performance characteristics { #perf }

| Kind | Build cost | Space | Query cost | Used by today |
|---|---|---|---|---|
| `.idx.zone` | O(n) one pass | O(1) — min/max/null-count | O(1) range check | filter comparisons (EQ/NE/LT/LE/GT/GE, int + float) |
| `.idx.hash` | O(n) one pass | O(n) — table + chain | O(matches) average; chain-walk on collisions | filter EQ (integer, null-free), filter IN, find |
| `.idx.sort` | O(n log n) | O(n) — row-id permutation | O(log n) binary search | filter range (EQ/LT/LE/GT/GE, selectivity guard), ORDER BY (single ASC key), distinct |
| `.idx.bloom` | O(n · k), k = 3 | O(m) bits, m default 64 | O(k) probe with false-positive rate | filter EQ definite-absent (integer-family only) |
| HNSW | O(n log n) typical | O(n · M) graph edges | O(log n) approximate | **Consulted directly** by `(ann)` |
| Linked column | O(n) one-time bind | O(n) row-id vector | O(1) deref | **Consulted directly** on column dereference |
| Partition pruning | None — layout-driven | None | O(p) partition count | **Optimizer pass** rewrites filters to skip non-matching partitions |

Every `.idx.*` fast path falls back to the linear scan on any miss (wrong kind, stale index, selectivity guard, HAS_NULLS, ineligible type, etc.) and returns identical results — verified by drop-differential tests in `test/rfl/ops/idx_route.rfl`.

## Next steps

- [**Indexes Overview**](../queries/indexes-overview.md) — the full landscape and decision matrix.
- [**Accelerator Indexes**](../queries/indexes.md) — per-function reference for `.idx.*`, including the routing table, eligibility conditions, and observability.
- [**Vector Search & HNSW**](../graph/vector-search.md) — ANN reference, persistence, metrics.
- [**Linked Columns**](../queries/links.md) — cross-table references.
- [**Storage Guide**](storage.md) — partitioned-table layout and recommended directory shapes.
