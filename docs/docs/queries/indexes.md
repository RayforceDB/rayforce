# Accelerator Indexes

Per-column accelerator structures — `zone`, `hash`, `sort`, `bloom` — attached directly to numeric vectors.  Build once at user request; survive copy / refcount semantics; ride alongside the column through the pipeline.

!!! note "v1 status"
    The four index kinds, their build kernels, and the `(.idx.*)` Rayfall surface are shipped.  The executor consults indexes at six sites: filter comparisons, filter IN, ORDER BY, distinct, find, and equi-joins.  See the [routing table](#routing-table) below.

This page is the **reference** for the `.idx.*` family. For the broader index landscape (HNSW, linked columns, partition pruning, CSR), see [Indexes Overview](indexes-overview.md). For decision walk-throughs and worked workflows, see the [Indexes Guide](../guides/indexes.md).

## Routing Table

The executor consults indexes at six sites.  Each site has a specific consumption point, eligibility contract, and fallback guarantee — results never differ from the scan path (verified by drop-differential tests in `test/rfl/ops/idx_route.rfl`).

| Index kind | Consumption site | What happens |
|---|---|---|
| `.idx.zone` | `filter` — comparison predicates (EQ/NE/LT/LE/GT/GE) | O(1) all/none short-circuit: if the constant falls outside the column's [min, max] range the filter returns 0 rows without scanning; if every row must pass the whole table is returned.  Integer and float families both supported. |
| `.idx.bloom` | `filter` — EQ on integer-family columns | Definite-absent proof: if all k probe bits are clear the key is absent and the filter returns 0 rows without scanning.  False positives fall through to the scan; there are no false absences. |
| `.idx.hash` | `filter` — EQ on integer-family, `SYM`, `STR` and `GUID` columns | Direct rowsel from hash-chain probe: matching rows are collected in O(matches) without a full column scan.  Integer columns must be null-free (HAS_NULLS → scan); a `SYM` or `STR` column may hold nulls, since a nonzero symbol or a non-empty string never matches one.  A `STR` probe hashes the literal's bytes and confirms each hit against them; `(== s "")` (the `STR` null, whose rows the index leaves out) scans.  A generation-2 (compressed) `STR` column's saved hash is probed where it is mapped: each hit is confirmed by reading that one cell, so the column is never decompressed or loaded whole.  A `GUID` probe hashes the literal's 16 bytes and confirms each hit against them; a null guid needle scans (the index keys null cells, but `==` and `in` in a `where:` never select them through it).  A generation-2 `GUID` column is materialized for its hash, as for `in`. |
| `.idx.hash` on each partition's column | `filter` — EQ and IN on a column of a partitioned table (`.db.parted.get`), every type the unpartitioned sites take | After partition pruning, each remaining partition's own hash answers its rows, generation 0 or 2 (a generation-2 `GUID` partition is materialized for its hash, as unpartitioned), and the answers are joined in partition order, so rows come back as the scan returns them.  An empty partition adds no rows.  A partition whose column has no index, or whose index is stale (built for another length), is scanned for its own rows, read a vector at a time as the scan reads it (a compressed partition is never materialized), while the others still probe.  The whole filter falls back to the scan when no remaining partition has a usable index, when a partition is missing the column, when a partition must be scanned and a needle is null (`""`, the null guid, a null integer) or a `STR` or `GUID` set holds more than 64 values, or for a float column.  `(== s "")` always scans, as unpartitioned.  `.idx.uses` counts one use for each partition whose index answered. |
| `part` backing (`.attr.set 'parted`) | `filter` — EQ predicates | Locate the value block and build the rowsel directly from its contiguous `[start, len)` span, including dense blocks. |
| `.idx.sort` | `filter` — range predicates (EQ/LT/LE/GT/GE) | Binary search over the row-id permutation to identify the matching span, then a rowsel is built from that span.  NE (two disjoint spans) falls back.  A selectivity guard fires when the span exceeds n/128 (~0.78%) — see [Performance Characteristics](#performance-characteristics) for the measured rationale. |
| `.idx.hash` | `filter` — IN predicates on integer-family, `SYM`, `STR` and `GUID` columns | Hash-chain probe for each set element; result is the union rowsel over all matches.  A `STR` set probes by each element's bytes; a set holding `""` scans.  A `GUID` set probes by each element's 16 bytes; a set holding the null guid scans.  On a generation-2 `STR` column the saved hash is probed where it is mapped, as for EQ. |
| `part` backing (`.attr.set 'parted`) | `filter` — IN predicates | Match partition keys once and build the rowsel from their contiguous physical spans. |
| `.idx.hash` / `part` backing | `where key IN …` + `group by key` | Resolve only the requested key slices and aggregate them directly, skipping filter scan and group discovery. Part slices stream as contiguous ranges. |
| `.idx.sort` | ORDER BY — single ascending key | The pre-built permutation is reused directly rather than re-sorting.  Descending ORDER BY falls back to recompute (reversing the perm would swap tie-group positions relative to stable DESC sort). |
| `.idx.sort` | `distinct` | Walk the sort permutation once to collect first-occurrence row ids per distinct value — O(n) with no hash table.  SYM/GUID/STR columns fall back. |
| `.idx.hash` | `find` | Hash-chain probe returns the minimum row id matching the needle, or a provably-absent signal.  Integer-family, `SYM`, `STR` and `GUID` needles against a column of the same family; float and cross-family needles fall through to the scan.  A `GUID` index keys every row by its bytes, the null guid included, so a `GUID` column with nulls keeps its index and a null needle finds a null cell, as on the scan.  A **vector** of needles probes once per needle — nothing is built per call. |
| `.idx.hash` | `in` — `(in needles v)` with `v` indexed, and `(at d keys)` on a dict keyed by an indexed vector | One probe per needle, same type matrix as `find`.  Without an index, `find` / `in` hash whichever side is smaller and scan the other once. |
| `.idx.hash` on a left key column | `inner-join`, `semi-mask`, `anti-mask` — integer-family and `SYM` keys, one key column or several | The left rows that can match are the index's rows for the values the right side holds on that column (an append's delta included; `SYM` values looked up in the left column's domain); the path is taken only when 32 × the right side's rows + r × its candidates stays below the left side's rows, r being 32 for a join and 8 for a mask (each right row costs a lookup in the index, each candidate a gathered and checked row).  It is decided in two steps.  First, at no cost, from sizes and the index's own row and group counts: a right side with a row for every 2048 left rows or more is not tried, and the rule is applied to the candidates the index's mean rows per value predicts; a join declined here looks up nothing.  Then the chosen column's right values are counted exactly from the index, before any row is read, stopping as soon as the count breaks the rule; a skewed index, whose mean underrates a heavy group, can be declined there, having looked up at most every right key, which the 2048 bound keeps within a fraction of a percent of the join.  The candidates, in row order, then go through the same hash, probe and whole-key equality as the scan, so the pairs or the mask, and their order, are the scan's.  With several indexed key columns the one with the fewest candidates is probed.  Otherwise the join runs as without the index (the bitmap prefilter or the plain probe): a left, full or anti join; a float, `STR` or `GUID` key; a numeric left column that may hold nulls; a `SYM` right column holding the null (a null key matches the left's null keys, which the index leaves out); for a join (not a mask), a right side over 65536 rows, which takes the join's radix path. |
| `.idx.hash` | `distinct`, `count` of `distinct` — `GUID` columns | The hash's groups are the distinct guids and each group's first row is that guid's first occurrence: `distinct` gathers them in first-appearance order, `(count (distinct v))` is the group count.  Without an index a `GUID` `distinct` dedups on the 16 bytes (in parallel on large columns). |

### Eligibility conditions (uniform)

All six sites apply the same prechecks before consulting an index:

- **Fresh:** the index's `built_for_len` must match the column's current length (mismatch → stale → fallback).
- **No nulls (new paths):** `HAS_NULLS` set on the column → fallback for all paths except hash-EQ at the filter site, which also re-checks `HAS_NULLS` explicitly.
- **Partitioned columns:** a column of a partitioned table routes `==` and `in` through each partition's own hash, under the rules in the routing table; its other sites, and MAPCOMMON columns (the partition key), skip index routing.
- **Default table:** filter and IN sites only route columns from the query's default table (non-default table references are skipped).

A write to existing rows (`(alter 'v set i val)`, an `update`) drops the attached index, so the column has `HAS_INDEX` clear afterward; reattach explicitly after the write.  An append keeps a hash index and drops any other kind: `insert` (into a vector or a table's column, in place or not), `concat`, `(alter 'v concat vals)` and an `upsert` that adds keys extend the hash over the appended rows, a repeated key's rows in the hash's delta, which every site reads — see [Mutation Drops the Index](#mutation-drops-the-index).

### Fallback guarantee

Every fast path falls back to the full scan on any miss (wrong kind, stale index, selectivity guard, OOM, ineligible type, etc.).  The fallback path is exercised by the drop-differential pattern: each case in `test/rfl/ops/idx_route.rfl` is run twice — once with the index attached and once with `.idx.drop` — and the results are asserted identical.

### count(select ...) and other bypass shapes

Two count-specific fast paths (`try_count_simple_compare` and `ray_try_count_select_expr`) intercept `(count (select {where: pred}))` shapes **before** the exec-level OP_FILTER that hosts the index sites.  These count fast paths evaluate the predicate directly on the raw column without consulting the index.  To reach the indexed path, the outer expression must materialize the full table result — e.g. `(sum (at (select {from: T where: pred}) 'col))`.  ORDER BY, distinct, and find are invoked directly on the column vector and always reach the index routing layer regardless of how the result is consumed.

## Quick Pick

Match the shape of your *query*, not the shape of the data.

| Query shape | Kind | Active now |
|---|---|---|
| Constant predicate may fall outside the column's value range | `.idx.zone` | Yes — O(1) all/none short-circuit at the filter site |
| Repeated `==` / `in` / `find` | `.idx.hash` | Yes — O(matches) hash probe at filter EQ, filter IN, and find sites |
| Range queries, sorted output | `.idx.sort` | Yes — binary search at filter range site; permutation reuse at ORDER BY; distinct walks the permutation instead of hashing |
| Cheap probabilistic membership rejection | `.idx.bloom` | Yes — definite-absent proof at filter EQ site (integer-family only) |

## Why Per-Column Indexes

Rayforce's hot path is morsel-driven columnar execution — every operator scans 1024-element chunks of contiguous values.  That's already fast for full-column work, but it's still O(n) for needle-in-haystack queries: `filter (== col 42)` visits every row even when only one matches, `(in col big-set)` builds a rowsel from a full scan, `(find col k)` linear-scans.

An **accelerator index** is a precomputed data structure attached to the column itself.  The user pays the build cost once; subsequent queries against that column consult the index instead of the raw values at the applicable routing sites.

- `zone` — min/max plus null count (~32 bytes per column).  Prunes whole-column filter work when a constant predicate falls outside the range.
- `hash` — chained open-addressing table.  O(1) eq / in / find; stays useful for non-unique columns via the chain.
- `sort` — ascending row-id permutation.  Binary search for range predicates; pre-sorted ORDER BY; distinct without a hash table.
- `bloom` — m-bit probabilistic membership filter (k=3 hashes).  Cheap, small, no false negatives — EQ definite-absent rejection at the filter site.

## Observability

Set `RAY_IDX_STATS=1` before running a process (any value but the empty string and `0` turns it on).  At exit, per-site consult and hit counters are printed to stderr in the form:

```
idx_route filter_zone   consults=3 hits=2
idx_route filter_bloom  consults=1 hits=1
idx_route filter_hash   consults=5 hits=5
idx_route filter_part   consults=3 hits=3
idx_route filter_range  consults=4 hits=3
idx_route in            consults=2 hits=2
idx_route group-slice   consults=2 hits=2
idx_route find          consults=8 hits=7
idx_route sort          consults=6 hits=6
idx_route distinct      consults=2 hits=2
```

A consult means the site attempted to use the index; a hit means the fast path succeeded and the scan was bypassed.  Consult without hit means the index was present but the guard (selectivity, eligibility, stale) rejected it.

## Constructing an Index

Each kind has a one-arg attach builtin.  Attaching is idempotent — if a different kind is already attached, it's dropped first; mutation invalidates and clears it implicitly.

### Signature

`(.idx.zone v)   (.idx.hash v)   (.idx.sort v)   (.idx.bloom v)`

Attach an index of the named kind to a vector `v`.  Returns the column with the new index attached (the underlying values are unchanged).  All kinds accept the numeric/boolean types `RAY_BOOL`, `RAY_U8`, `RAY_I16`, `RAY_I32`, `RAY_I64`, `RAY_F32`, `RAY_F64`, `RAY_DATE`, `RAY_TIME`, `RAY_TIMESTAMP`; `.idx.hash` **additionally** accepts `RAY_SYM` (keyed on domain ids), `RAY_STR` and `RAY_GUID` (each keyed on a 64-bit hash of the bytes, payload compared on every hit).  `RAY_SYM` / `RAY_STR` / `RAY_GUID` on the non-hash kinds is deferred to v2.

### Examples

```lisp
(set v [5 1 9 3 7])

; Zonemap: min, max, null count.
(set vz (.idx.zone v))
(.idx.info vz)
; ⇒ {kind:zone length:5 parent_type:5 saved_attrs:0 min:1 max:9 n_nulls:0}

; Hash: chained table with the linked-list of duplicates.
(set vh (.idx.hash v))
(.idx.info vh)
; ⇒ {kind:hash length:5 parent_type:5 saved_attrs:0 capacity:16 n_keys:5}

; Sort: ascending row-id permutation.
(set vs (.idx.sort v))
(.idx.info vs)
; ⇒ {kind:sort length:5 parent_type:5 saved_attrs:0 perm_len:5}

; Bloom: m bits, k hashes, n_keys non-null rows added.
(set vb (.idx.bloom v))
(.idx.info vb)
; ⇒ {kind:bloom length:5 parent_type:5 saved_attrs:0 m_bits:64 k:3 n_keys:5}
```

Indexed columns participate in normal operations transparently.  `(sum vh)`, `(at vz 0)`, `(count vb)` all work identically to the un-indexed source.  Whatever was true of `v` — nullability, slicing, arithmetic — is true of every `vz`, `vh`, `vs`, `vb`.

## Introspection & Lifecycle

| Function | Returns | Notes |
|---|---|---|
| `(.idx.has? v)` | boolean | True iff any index kind is currently attached. |
| `(.idx.info v)` | dict or null | Per-kind metadata: kind, length, parent_type, saved_attrs, plus kind-specific stats (min/max, capacity, perm_len, m_bits / k / n_keys).  Null when no index is attached. |
| `(.idx.drop v)` | v with index removed | Detaches whichever kind is attached; restores the underlying nullmap state byte-for-byte.  No-op if no index is attached. |

### Mutation Drops the Index

A write to existing rows — `(alter 'v set i val)` — invalidates the attached index.  The mutator paths drop the index transparently, restore the original nullmap bytes, and proceed with the write.  Subsequent `(.idx.has?)` calls return false; reattach the index after the write if you want index routing on the updated column.

An append keeps a **hash** index (any other kind is dropped): `(insert 'v x)` of one value, `(insert 't rows)` on a table's indexed column, their non-quoted forms, `concat` and `(alter 'v concat vals)` extend it over the new rows, probing each once — the old rows are not rehashed.  `(insert 'v vals)` of several values into a plain vector, quoted or not, builds a fresh vector without the index (`(alter 'v concat vals)` appends them and keeps it), and an insert at a position, `(insert 'v i vals)`, is a write that drops it.  A new key becomes a group of its own.  A row under a key the hash already holds goes to the hash's **delta**, a small side table of appended rows, instead of into the key's group (which would re-lay every row).  The hash stays complete: every site in the routing table reads a key's rows as its group followed by its delta rows, which are always later rows, so the answers and their order are those of a freshly built hash.  Once the delta holds more than 4096 rows and more than half the grouped rows, it is folded into the groups in one pass over the rows, about three row copies per appended row amortized; a single large append folds as it goes.  What that costs an append: a batch that does not fold costs what an append to a hash-indexed column cost before (on 900-row batches of repeated symbols into 351 000 and 2 000 000 rows, 11 µs and 15 µs against 13.5 µs and 15.1 µs), and a batch that folds pays one pass over the index's rows (1.2 ms at 351 000 rows, 2.7 ms at 2 000 000), so averaged over every batch an append to an indexed column costs about a sixth to a quarter more than when an append left the index first-row-only, and the queries after it read the index instead of scanning.  An append to a column without an index costs what it did.  A save writes the hash with its delta folded in.  `.idx.info` reports `n_keys` as every indexed row, delta included (the number a fresh `.idx.hash` reports), and `delta` as the rows waiting to be folded (0 when none); `.attr.get` keeps `grouped`, and drops `unique` as soon as an append repeats a key.  A null appended to a numeric column drops the hash; a `SYM` or `STR` null row is left out of it, as the build leaves it out.  An `upsert` that adds keys extends the hash on every column it appends to; one that rewrites matched cells drops the index of each column it rewrites, as any write does (its key columns are never rewritten).  An append to a column whose hash is shared (another binding holds it, or it was loaded from a file) copies it, delta included, once; the other holder keeps its own.

```lisp
(set v [5 1 9 3 7])
(set vz (.idx.zone v))
(.idx.has? vz)              ; ⇒ true
(alter 'vz set 0 100)
(.idx.has? vz)              ; ⇒ false

(set t (table [k] (list (.idx.hash [10 20 30]))))
(insert 't (table [k] (list [40])))
(.idx.has? (at t 'k))                          ; ⇒ true   — a new key: a group of its own
(insert 't (table [k] (list [20])))
(at (.idx.info (at t 'k)) 'delta)              ; ⇒ 1      — a repeat, in the delta
(at (.idx.info (at t 'k)) 'n_keys)             ; ⇒ 5
(find (at t 'k) [20 40])                       ; ⇒ [1 3]  — still through the hash
(select {from: t where: (== k 20)})            ; both rows of 20, through the hash
```

### Coexistence with Linked Columns

An accelerator index uses bytes 0–7 of the column's [nullmap union](https://github.com/RayforceDB/rayforce/blob/master/include/rayforce.h); a [linked column](links.md) uses bytes 8–15.  The two flags — `RAY_ATTR_HAS_INDEX = 0x08` and `RAY_ATTR_HAS_LINK = 0x04` — are independent and a column can carry both.  Dereferencing through a link still works on an indexed column; dropping the index leaves the link intact and vice versa.

## Common Workflows

### Hot column, repeated point lookups

```lisp
(set v (* 7 (til 1000)))   ; 0, 7, 14, ..., 6993
(set v (.idx.hash v))
(in 700 v)                   ; ⇒ true   (700 = 7 × 100) — hash-chain probe
(in 701 v)                   ; ⇒ false
(find v 700)                 ; ⇒ 100    — hash fast path returns minimum row id
(find v [700 701 6993])      ; ⇒ [100 0N 999] — one probe per needle
(in [700 701] v)             ; ⇒ [true false]
(set v (.idx.drop v))         ; optional early release; mutators auto-drop
```

An append keeps the hash: `(concat v [7000 7007])` carries the index over, and so does `(insert 'v 7014)` in place; an appended repeat goes to its delta, and every site keeps using it (see [Mutation Drops the Index](#mutation-drops-the-index)).

### Range scans on a stable column

```lisp
(set prices [102 98 105 101 99 103])
(set prices (.idx.sort prices))
(.idx.info prices)
; ⇒ {kind:sort length:6 parent_type:5 saved_attrs:0 perm_len:6}
```

The original column is unchanged; the sort lives in a row-id permutation alongside it.  Range queries against this column use binary search over the permutation rather than a full scan — provided the span stays within the ~0.78% selectivity threshold.

### Whole-column pruning

```lisp
(set ages [21 34 29 42 37])
(set ages (.idx.zone ages))
(.idx.info ages)
; ⇒ {kind:zone length:5 parent_type:5 saved_attrs:0 min:21 max:42 n_nulls:0}
```

A predicate like `(== ages 17)` falls outside `[21, 42]`; the executor short-circuits to 0 rows without scanning.  A predicate like `(>= ages 1)` sees every row must pass and returns the full table without scanning.

## Performance Characteristics

| Kind | Build | Space | Query cost | Selectivity guard |
|---|---|---|---|---|
| `.idx.zone` | O(n) one pass | O(1) — min, max, n_nulls | O(1) range check | None — all/none only |
| `.idx.hash` | O(n) | O(n) table + chain | O(matches) average; chain-walk on collisions | None |
| `.idx.sort` | O(n log n) | O(n) row-id permutation | O(log n) binary search + O(m log m) rowsel | Span > n/128 (~0.78%) → fallback |
| `.idx.bloom` | O(n · k), k = 3 | O(m) bits, m = 64 default | O(k) probe | EQ only; integer family only |

**Sort-index selectivity guard:** the rowsel build after the binary search costs O(m log m) where m is the match count.  Measured break-even on shuffled data is 0.5–1% selectivity; the guard threshold of n/128 (~0.78%) keeps the fast path in the win region and rejects the loss region.  The guard is skipped when n < 64 (trivial case).  See `bench/bottleneck/idx_route_compare.md` ROUND 2 Q1 for the curve.

## Storage and Lifetime

An accelerator index is a child `ray_t*` of type `RAY_INDEX` whose `data[]` payload holds a `ray_index_t` struct — the kind tag, kind-specific child references (the hash table + chain vec, the sort permutation vec, the bloom bit vec), and a 16-byte snapshot of the parent's pre-attach nullmap union.  When the index is attached, the parent's nullmap union holds an owning pointer to the index ray_t.  Detach memcpy's the snapshot back; refcounting handles shared cases (a COW-shared index is cloned-on-detach so neither holder's view breaks).

This design has three useful properties:

- **Free for non-indexed columns.**  Vectors without `RAY_ATTR_HAS_INDEX` set pay no extra cost — the same 16 bytes that hold the index pointer hold the original inline nullmap or external pointer, which they would have anyway.
- **Survives copies and slices.**  `ray_alloc_copy` retains the index along with the parent; slicing creates a fresh slice header that doesn't itself carry `HAS_INDEX`, so the slice is index-free until you attach one to it.  The parent and any other holder still see the original index.
- **Dropped, not orphaned, on mutation.**  Indexes are transient by design.  Persistence is intentionally not supported (use `(.col.link)` for persistent column metadata) — `ray_col_save` serializes a clean column with no index, the on-disk format never carries the bit.

## Caveats and Limits

- **Type support.**  All four kinds accept `RAY_BOOL`, `RAY_U8`, `RAY_I16`, `RAY_I32`, `RAY_I64`, `RAY_F32`, `RAY_F64`, `RAY_DATE`, `RAY_TIME`, `RAY_TIMESTAMP`; `.idx.hash` additionally accepts `RAY_SYM` (indexing the symbol domain ids), `RAY_STR` and `RAY_GUID` (indexing a hash of each row's bytes).  `RAY_SYM`, `RAY_STR` and `RAY_GUID` on the zone/sort/bloom kinds are not yet supported.
- **One slot per column.**  A column carries at most one index kind at a time.  Calling a different kind's attach replaces the existing one.  Multiple indexes per column are deferred — the slot can point to a list-of-indexes in v2 if real workloads want both eq and range fast paths.
- **In-memory only.**  Indexes do not persist across `ray_col_save` / `ray_col_load`; users rebuild explicitly after a load.  Unlike the HNSW handle (see [Vector Search](../graph/vector-search.md)), which has dedicated `.emb.hnsw.set` / `.emb.hnsw.get` builtins for explicit persistence, `.idx.*` has no on-disk format at all — rebuild is the only option.
- **Float equality not indexed.**  Hash-EQ and hash-IN routes are integer-family only — float equality has NaN / -0 semantics that the unfused compare kernel handles; the index paths intentionally don't replicate that.
- **NE predicates fall back.**  The sort-index range site does not handle NE (two disjoint spans); the scan handles it.
- **Null-bearing columns fall back.**  All new routing paths gate on `HAS_NULLS`; hash-EQ at the filter site also re-checks HAS_NULLS explicitly.  Null-bearing columns never receive index routing in v1.

## Quick Reference

| Function | Syntax | Description |
|---|---|---|
| `.idx.zone` | `(.idx.zone v)` | Attach a min/max + null-count zonemap |
| `.idx.hash` | `(.idx.hash v)` | Attach a chained hash table for eq lookups |
| `.idx.sort` | `(.idx.sort v)` | Attach an ascending row-id permutation |
| `.idx.bloom` | `(.idx.bloom v)` | Attach an *m*-bit Bloom filter |
| `.idx.drop` | `(.idx.drop v)` | Remove any attached index |
| `.idx.has?` | `(.idx.has? v)` | True iff `v` carries an index |
| `.idx.info` | `(.idx.info v)` | Dict of kind + length + per-kind stats, or null |
