# `.emb.*` — embeddings and vector search

Distances between numeric vectors, exact nearest neighbours, and the HNSW index for approximate ones. An embedding column is a list whose entries are numeric vectors of one length; see [Vector Search & HNSW](../graph/vector-search.md) for the worked reference, persistence and the `nearest:` clause of `select`.

These functions were global names (`cos-dist`, `knn`, `hnsw-build`, …) before they were gathered here. The old names are gone. `norm` is linear algebra and stays a global name, beside `mmu`, `inv` and the rest.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.emb.cos-dist`](#emb-cos-dist) | binary | — | Cosine distance between two vectors. |
| [`.emb.inner-prod`](#emb-inner-prod) | binary | — | Inner product of two vectors. |
| [`.emb.l2-dist`](#emb-l2-dist) | binary | — | Euclidean distance between two vectors. |
| [`.emb.knn`](#emb-knn) | variadic | — | Exact k nearest neighbours by brute force. |
| [`.emb.hnsw-build`](#emb-hnsw-build) | variadic | — | Build an HNSW index over an embedding column. |
| [`.emb.ann`](#emb-ann) | variadic | — | Approximate k nearest neighbours through an HNSW index. |
| [`.emb.hnsw-info`](#emb-hnsw-info) | unary | — | Dict describing an index. |
| [`.emb.hnsw-save`](#emb-hnsw-save) | binary | restricted | Persist an index to a directory. |
| [`.emb.hnsw-load`](#emb-hnsw-load) | unary | restricted | Load an index from a directory. |
| [`.emb.hnsw-free`](#emb-hnsw-free) | unary | restricted | Release an index early. |

## `.emb.cos-dist` { #emb-cos-dist }

Signature: `(.emb.cos-dist a b)`. Cosine distance, `1 - cos(a, b)`: lower is closer. Both operands are numeric vectors of the same length.

```lisp
(.emb.cos-dist [1.0 0.0] [0.0 1.0])   ;; => 1.0
```

## `.emb.inner-prod` { #emb-inner-prod }

Signature: `(.emb.inner-prod a b)`. The inner product: higher is closer.

## `.emb.l2-dist` { #emb-l2-dist }

Signature: `(.emb.l2-dist a b)`. Euclidean distance: lower is closer.

## `.emb.knn` { #emb-knn }

Signature: `(.emb.knn col query k [metric])`. Scans every vector of `col` and returns a table `{_rowid, _dist}` of the `k` nearest to `query`, ascending by distance. `metric` is `'cosine` (default), `'l2` or `'ip`. No index is needed; it is the right tool for small columns and one-off queries.

```lisp
(.emb.knn vecs [0.1 0.2 0.3 0.4] 2)
```

## `.emb.hnsw-build` { #emb-hnsw-build }

Signature: `(.emb.hnsw-build col [metric] [M] [ef_construction])`. Returns an index handle. Defaults: `'cosine`, `M=16`, `ef_construction=200`.

## `.emb.ann` { #emb-ann }

Signature: `(.emb.ann handle query k [ef_search])`. Returns `{_rowid, _dist}` for the approximately `k` nearest, ascending by distance.

```lisp
(set h (.emb.hnsw-build vecs 'cosine 16 200))
(.emb.ann h [0.1 0.2 0.3 0.4] 2 50)
```

## `.emb.hnsw-info` { #emb-hnsw-info }

Signature: `(.emb.hnsw-info handle)`. A dict: `{nrows dim metric nlayers M efc}`.

## `.emb.hnsw-save` { #emb-hnsw-save }

Signature: `(.emb.hnsw-save handle path)`. Writes the index to a directory; the metric is kept.

## `.emb.hnsw-load` { #emb-hnsw-load }

Signature: `(.emb.hnsw-load path)`. Returns a handle to the index saved at `path`.

## `.emb.hnsw-free` { #emb-hnsw-free }

Signature: `(.emb.hnsw-free handle)`. Releases the index now. Optional: a handle is reference-counted and is released when the last binding to it goes.
