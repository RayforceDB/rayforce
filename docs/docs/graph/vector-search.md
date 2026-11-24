# Vector Search

Embedding similarity, brute-force KNN, and HNSW-accelerated approximate nearest-neighbour search — composable with Rayfall's `select` for filter-aware retrieval.

## Data Model

An embedding column is a `RAY_LIST` whose entries are numeric vectors (preferably `RAY_F32`) of the same length `D`. Tables hold embeddings alongside any other column types — integers, strings, dates, symbols — and all columns project correctly through nearest-neighbour queries.

```lisp
(set docs (table [id title score emb]
    (list [0 1 2 3 4]
          (list "alpha" "beta" "gamma" "delta" "epsilon")
          [0.9 0.2 0.8 0.1 0.7]
          (list [1.0 0.0 0.0] [0.0 1.0 0.0]
                [0.0 0.0 1.0] [1.0 1.0 0.0]
                [1.0 0.0 1.0]))))
```

## Direct Distance & Similarity

Four direct builtins. All accept either *two vectors* (returning a scalar) or *a LIST-of-vectors plus a query vector* (returning a vector of per-row results).

| Function | Returns | Range |
|---|---|---|
| `(.emb.cos-dist a b)` | cosine distance `1 − cos(a, b)` | `[0, 2]` — lower = closer |
| `(.emb.l2-dist a b)` | euclidean distance `||a − b||₂` | `[0, ∞)` — lower = closer |
| `(.emb.inner-prod a b)` | dot product `∑ aᵢ·bᵢ` | any real |
| `(norm x)` | L2 norm `||x||₂` | `[0, ∞)` |

```lisp
(.emb.cos-dist [1.0 0.0] [0.0 1.0])         ; → 1.0 (orthogonal)
(.emb.l2-dist  [0.0 0.0] [3.0 4.0])         ; → 5.0
(.emb.inner-prod [1.0 2.0] [3.0 4.0])      ; → 11.0
(norm    [3.0 4.0])                 ; → 5.0
```

## Brute-force K-Nearest-Neighbours

`(.emb.knn col query k [metric])` scans a full LIST column and returns the top-K rows as a table with `_rowid` and `_dist` columns (ascending by distance). The metric defaults to `'cosine`; also supports `'l2` and `'ip` (inner-product, sorted by `−dot` so lower = closer matches the other metrics).

```lisp
(.emb.knn (at docs 'emb) [1.0 0.0 0.0] 3 'cosine)
; ┌────────┬───────┐
; │ _rowid │ _dist │
; │  I64   │  F64  │
; ├────────┼───────┤
; │ 0      │ 0.0   │
; │ 4      │ 0.29  │
; │ 3      │ 0.29  │
; └────────┴───────┘
```

## HNSW Indexes

Build a hierarchical navigable small-world graph over an embedding column for sub-linear nearest-neighbour queries. All three metrics are indexed natively and the chosen metric persists through save/load.

### Building

```lisp
; (.emb.hnsw-build col [metric] [M] [ef_construction])
(set idx (.emb.hnsw-build (at docs 'emb) 'cosine 16 100))
```

Defaults: `metric='cosine`, `M=16`, `ef_construction=200`. The returned handle is an atom tagged `RAY_ATTR_HNSW`; the underlying `ray_hnsw_t*` is rc-managed — rebinding, scope exit, and process teardown all release the index automatically.

### Querying

```lisp
; (.emb.ann handle query k [ef_search])
(.emb.ann idx [1.0 0.0 0.0] 3)
; ┌────────┬───────┐
; │ _rowid │ _dist │
; ├────────┼───────┤
; │ 0      │ 0.0   │
; │ 3      │ 0.29  │
; │ 4      │ 0.29  │
; └────────┴───────┘
```

### Persistence & Lifecycle

```lisp
(.emb.hnsw-save idx "/tmp/rayforce-docs-hnsw")
(set idx2 (.emb.hnsw-load "/tmp/rayforce-docs-hnsw"))
(.emb.hnsw-info idx2)     ; dict: {nrows dim metric nlayers M efc}
(.emb.hnsw-free idx2)     ; optional — rc-managed, will free on scope exit too
```

## Filter-Aware ANN via `select … nearest`

The `select` form accepts a `nearest:` clause that composes WHERE filtering with HNSW-backed (or brute-force) top-K retrieval in one query. The filter predicate is pushed into HNSW's beam search as an iterative scan — rejected candidates are still traversed for graph connectivity but don't consume result slots.

```lisp
(select {from: docs
         where: (> score 0.5)
         nearest: (.emb.ann idx [1.0 0.0 0.0])
         take: 10})
; Returns top-10 rows of `docs` whose score > 0.5, ordered by cosine
; distance to the query.  The implicit projection returns source
; columns only; to include _dist, name it in the output projection.
```

Brute-force variant on a column reference (no pre-built index needed):

```lisp
(select {from: docs
         nearest: (.emb.knn emb [1.0 0.0 0.0])
         take: 3})
```

### Projection & `_dist`

The rerank step emits a synthetic `_dist` column, but it is **not** included in the default output — `(select {from: t nearest: …})` returns the source schema exactly, preserving shape compatibility with `(select {from: t})`. To include the distance, reference `_dist` in an explicit projection:

```lisp
(select {id: id title: title d: _dist
         from: docs
         where: (> score 0.5)
         nearest: (.emb.ann idx [1.0 0.0 0.0])
         take: 5})
```

### Metric Symbols

Accepted by `.emb.hnsw-build`, `.emb.knn`, and the `nearest` clause:

- `'cosine` — `1 − cos(a, b)`, lower = closer (default)
- `'l2` — euclidean distance, lower = closer
- `'ip` — negated inner product, lower = closer

## Reference

| Name | Arity | Signature |
|---|---|---|
| `.emb.cos-dist` | 2 | `(.emb.cos-dist a b)` — cosine distance; vec×vec → atom, LIST×vec → vector |
| `.emb.l2-dist` | 2 | `(.emb.l2-dist a b)` — euclidean distance |
| `.emb.inner-prod` | 2 | `(.emb.inner-prod a b)` — positive dot product |
| `norm` | 1 | `(norm x)` — L2 norm; scalar if vec, per-row vector if LIST |
| `.emb.knn` | 3–4 | `(.emb.knn col query k [metric])` — brute-force top-K |
| `.emb.hnsw-build` | 1–4 | `(.emb.hnsw-build col [metric] [M] [ef_c])` — build index |
| `.emb.ann` | 3–4 | `(.emb.ann handle query k [ef_s])` — approximate top-K via HNSW |
| `.emb.hnsw-save` | 2 | `(.emb.hnsw-save handle path)` — persist index to directory |
| `.emb.hnsw-load` | 1 | `(.emb.hnsw-load path)` — restore index from directory |
| `.emb.hnsw-free` | 1 | `(.emb.hnsw-free handle)` — explicit release (rc-managed, optional) |
| `.emb.hnsw-info` | 1 | `(.emb.hnsw-info handle)` — dict of `{nrows dim metric nlayers M efc}` |

## Design Notes

- **Distance convention.** All metrics encode as "lower = closer" for consistent sort ordering. Direct math builtins (`cos-sim`-style semantics) aren't provided separately — compute `(- 1.0 (.emb.cos-dist a b))` if needed.
- **Handle lifecycle.** HNSW handles are rc-managed through Rayforce's ownership semantics: a deep clone is produced on `ray_cow`, ownership transfers via detach on realloc, and the underlying index is freed when the last reference drops. `(.emb.hnsw-free h)` is idempotent and not required for correctness.
- **Iterative scan bounds.** Filter-aware ANN pushes the predicate into the beam but remains locality-bounded by HNSW's graph reachability from the query's entry point. For pathologically selective filters on clustered data, results may be fewer than K; multi-entry restart is future work.
- **Allocation-failure contract.** All HNSW search entry points return a distinct sentinel (`-1`) on allocation failure — every caller surfaces this as `ray_error("oom", …)` rather than a silently-empty result.
