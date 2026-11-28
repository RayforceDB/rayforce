# `.graph.*` — graph algorithms

Builds a CSR-backed graph from an edge table, then exposes a family of algorithms — traversal, shortest paths, centrality, community detection, spanning trees — that consume the graph handle and return tables keyed by node ID. The graph is an opaque `i64` atom with the `RAY_ATTR_GRAPH` bit set; it owns a `ray_rel_t*` that must be released with [`.graph.free`](#graph-free) when no longer needed.

All algorithm wrappers follow the same template: validate the handle, fold positional parameters off `args[]`, build a one-node DAG, dispatch through `ray_execute`, return the result table. Algorithms that consume edge weights read the `'weight` column registered at build time.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.graph.build`](#graph-build) | variadic | — | Build a graph handle from an edge table |
| [`.graph.free`](#graph-free) | unary | — | Release a graph handle (idempotent) |
| [`.graph.info`](#graph-info) | unary | — | Dict of n_nodes / n_edges / sorted / has_weights |
| [`.graph.expand`](#graph-expand) | variadic | — | 1-hop neighbour expansion from a seed node |
| [`.graph.var-expand`](#graph-var-expand) | variadic | — | BFS variable-depth expansion with optional path tracking |
| [`.graph.dfs`](#graph-dfs) | variadic | — | Depth-first traversal from a seed node |
| [`.graph.random-walk`](#graph-random-walk) | variadic | — | Uniform random walk from a seed node |
| [`.graph.shortest-path`](#graph-shortest-path) | variadic | — | BFS shortest path between two nodes |
| [`.graph.dijkstra`](#graph-dijkstra) | variadic | — | Weighted shortest paths from a source (optionally to a target) |
| [`.graph.k-shortest`](#graph-k-shortest) | variadic | — | Yen's k shortest weighted paths between two nodes |
| [`.graph.pagerank`](#graph-pagerank) | variadic | — | Iterative PageRank scores |
| [`.graph.degree`](#graph-degree) | variadic | — | In/out/total degree per node |
| [`.graph.betweenness`](#graph-betweenness) | variadic | — | Brandes betweenness centrality (exact or sampled) |
| [`.graph.closeness`](#graph-closeness) | variadic | — | Closeness centrality (exact or sampled) |
| [`.graph.connected`](#graph-connected) | variadic | — | Connected components via label propagation |
| [`.graph.louvain`](#graph-louvain) | variadic | — | Louvain modularity-based community detection |
| [`.graph.cluster`](#graph-cluster) | variadic | — | Local clustering coefficient per node |
| [`.graph.mst`](#graph-mst) | variadic | — | Minimum spanning tree (Kruskal) |
| [`.graph.topsort`](#graph-topsort) | variadic | — | Topological sort (Kahn) |

## `.graph.build` { #graph-build }

Signatures:

- `(.graph.build tbl 'src 'dst)` — unweighted graph.
- `(.graph.build tbl 'src 'dst 'weight)` — weighted graph; the weight column must be numeric (`I64`/`I32`/`F32`/`F64` — coerced to `F64` internally).

`src`/`dst` columns can be `I64`/`I32`/`I16`/`U8`/`SYM` — non-`I64` columns are widened to `I64` into a scratch vector. The node universe is `max(max(src), max(dst)) + 1`.

Returns: an opaque graph handle. Free with `.graph.free` when done — leaking it keeps the CSR alive (it may be tens of MB for large graphs).

```lisp
(set edges (table [src dst w]
  (list [0 0 1 1 2 2 3 4]
        [1 2 2 3 3 4 5 5]
        [1.0 4.0 2.0 5.0 1.0 3.0 2.0 1.0])))

(set g (.graph.build edges 'src 'dst 'w))
```

Errors: `rank` (arity != 3 or 4), `type` (tbl not a table / column refs wrong type), `name` (column not in tbl), `length` (src/dst/weight len mismatch), `oom`.

## `.graph.free` { #graph-free }

Signature: `(.graph.free g)`. Releases the underlying CSR. A second call returns a `type` error because the `RAY_ATTR_GRAPH` bit is cleared on the first call; it does not double-free.

```lisp
(set gtmp (.graph.build edges 'src 'dst 'w))
(.graph.free gtmp)
```

## `.graph.info` { #graph-info }

Signature: `(.graph.info g)`. Returns a dict with keys `n_nodes`, `n_edges`, `sorted` (bool: true when adjacency lists are sorted, required for LFTJ-style joins), `has_weights`.

```lisp
(.graph.info g)
;; => {n_nodes: 6, n_edges: 8, sorted: true, has_weights: true}
```

## `.graph.expand` { #graph-expand }

Signature: `(.graph.expand g src [direction])`. `direction` is `0` (forward, default), `1` (reverse), `2` (both). Returns a `{_src, _dst}` table — direct neighbours of `src` in the chosen direction.

```lisp
(count (.graph.expand g 0))      ;; forward neighbours of node 0
(set rev (.graph.expand g 5 1))  ;; reverse neighbours of node 5
```

Errors: `rank`, `type`, `domain` (direction out of {0,1,2}).

## `.graph.var-expand` { #graph-var-expand }

Signature: `(.graph.var-expand g src min-depth max-depth [direction] [track-path])`. BFS to between `min-depth` and `max-depth` hops, returning a node table. The current implementation returns endpoint/depth rows only; pass `track-path` as false or omit it. Depths are clamped to `[0, 255]` and `min-depth ≤ max-depth`.

```lisp
;; All nodes 1..3 hops from node 2, both directions
(.graph.var-expand g 2 1 3 2 0)
```

## `.graph.dfs` { #graph-dfs }

Signature: `(.graph.dfs g src [max-depth])`. Returns `{_node, _depth, _parent}` in DFS visit order. Default `max-depth` is 255.

## `.graph.random-walk` { #graph-random-walk }

Signature: `(.graph.random-walk g src [walk-len])`. Returns `{_step, _node}`. Default `walk-len` is 10.

Each step moves to one of the current node's out-neighbours, chosen uniformly (edge weights are not used); the walk stops early at a node with none. The choices come from the engine's random stream, the one [`rand`](../language/functions.md) draws from: successive walks from the same node differ, a fresh process gives the same walks in the same order on every platform and at any worker count, and a walk takes `walk-len` values of the stream (whether or not it stops early), so a `rand` or a walk advances the other.

## `.graph.shortest-path` { #graph-shortest-path }

Signature: `(.graph.shortest-path g src dst [max-depth])`. Bidirectional BFS — unweighted hop-count shortest path. Default `max-depth` is 255.

```lisp
(>= (count (.graph.shortest-path g 0 5)) 1)   ;; reachable
(count (.graph.shortest-path g 3 3))          ;; 1 (start = goal)
```

## `.graph.dijkstra` { #graph-dijkstra }

Signature: `(.graph.dijkstra g src [dst] [max-depth])`. With `dst` omitted (or `null`) it runs single-source mode, returning all reachable nodes. With `dst` set, it returns the path to that target. `max-depth` defaults to 255.

Requires that the graph was built with a weight column — otherwise returns `schema` error "graph has no weight column". Negative edge weights surface a `domain` error from the executor.

```lisp
;; Single-source distances from node 0
(set dst (.graph.dijkstra g 0))

;; Path from 0 to 5
(set path (.graph.dijkstra g 0 5))
```

## `.graph.k-shortest` { #graph-k-shortest }

Signature: `(.graph.k-shortest g src dst k)`. Yen's algorithm — finds up to `k` shortest weighted paths between `src` and `dst`. `k` must be in `[1, 65535]`. Requires a weight column.

## `.graph.pagerank` { #graph-pagerank }

Signature: `(.graph.pagerank g [iter] [damping])`. Defaults: `iter=30`, `damping=0.85`. Bounds: `iter ∈ (0, 65535]`, `damping ∈ (0, 1)`. Returns `{_node, _rank}`.

```lisp
(.graph.pagerank g)              ;; defaults
(.graph.pagerank g 100 0.9)      ;; more iterations, higher damping
```

## `.graph.degree` { #graph-degree }

Signature: `(.graph.degree g)`. Returns `{_node, _in_degree, _out_degree, _degree}`.

## `.graph.betweenness` { #graph-betweenness }

Signature: `(.graph.betweenness g [sample])`. `sample=0` (default) runs the exact Brandes algorithm; `sample>0` samples that many source nodes (faster, approximate). Bounds: `sample ∈ [0, 65535]`. Returns `{_node, _centrality}`.

## `.graph.closeness` { #graph-closeness }

Signature: `(.graph.closeness g [sample])`. Same `sample` semantics as betweenness. Returns `{_node, _centrality}`.

## `.graph.connected` { #graph-connected }

Signature: `(.graph.connected g)`. Label-propagation connected components — returns `{_node, _component}`. Treats the graph as undirected.

## `.graph.louvain` { #graph-louvain }

Signature: `(.graph.louvain g [max-iter])`. Modularity-maximising community detection. Default `max-iter=100`. Returns `{_node, _community}`.

## `.graph.cluster` { #graph-cluster }

Signature: `(.graph.cluster g)`. Local clustering coefficient per node — the fraction of each node's neighbours that are also connected to each other. Returns `{_node, _coefficient}`. Pendant nodes (degree < 2) get coefficient `0.0`.

## `.graph.mst` { #graph-mst }

Signature: `(.graph.mst g)`. Kruskal's minimum spanning forest. Requires a weight column. Returns `{_src, _dst, _weight}` — one row per MST edge.

## `.graph.topsort` { #graph-topsort }

Signature: `(.graph.topsort g)`. Kahn's topological sort. Returns `{_node, _order}` on a DAG; errors on a graph that contains a cycle.

## See also

- [Graph Algorithms](../graph/algorithms.md) — C API equivalents, opcode reference, complexity table, SIP and factorized execution.
- [Graph Storage](../graph/storage.md) — CSR construction details and the `ray_rel_t` lifetime contract.
- [Datalog Guide](../guides/datalog.md) — declarative reachability queries that often replace explicit traversal.
