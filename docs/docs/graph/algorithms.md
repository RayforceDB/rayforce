# Graph Algorithms

Graph opcodes covering traversal, shortest paths, centrality, community detection, similarity search, optimal joins, and spanning trees — all integrated into the DAG pipeline.

Graph algorithms are available through the C API. Many graph queries can also be expressed in Rayfall using the Datalog engine — see the [Graph Queries Tutorial](../tutorials/graph.md).

!!! note "All graph algorithms"
    All graph algorithms operate on `ray_rel_t` relationships built from CSR storage. See [Graph Storage](storage.md) for how to build and persist relationships.

## Traversal

### OP_EXPAND — 1-Hop Neighbor Expansion

Expands each input node to its direct neighbors in the CSR. The fundamental building block for all graph queries.

| Property | Value |
|---|---|
| Opcode | `OP_EXPAND (80)` |
| Complexity | O(d) per node, where d = average degree |
| Use case | Neighbor lookup, 1-hop reachability, join-like expansions |

```c
/* C API */
ray_op_t* nbrs = ray_expand(g, src_nodes, rel, 0);  /* direction: 0=fwd */
```

#### Rayfall

In Rayfall, 1-hop expansion is a Datalog query with a single pattern. Pin the source entity to query its direct neighbors:

```lisp
; Build a graph
(set db (datoms))
(set db (assert-fact db 0 'edge 1))
(set db (assert-fact db 0 'edge 2))
(set db (assert-fact db 1 'edge 2))

; 1-hop neighbors of node 0
(query db (find ?target)
  (where (0 :edge ?target)))
; => ?target: 1, 2
```

### OP_VAR_EXPAND — Variable-Length BFS/DFS

Expands nodes through multiple hops with configurable minimum and maximum depth. Uses BFS by default. Optionally tracks the full path for each discovered node.

| Property | Value |
|---|---|
| Opcode | `OP_VAR_EXPAND (81)` |
| Complexity | O(V + E) within the explored subgraph |
| Use case | Multi-hop reachability, friends-of-friends, transitive closure |

```c
/* BFS from start_nodes, depth 1..3, tracking paths */
ray_op_t* reachable = ray_var_expand(g, start_nodes, rel,
    0,    /* direction: forward */
    1,    /* min_depth */
    3,    /* max_depth */
    true  /* track_path */
);
```

#### Rayfall

Recursive Datalog rules express variable-length reachability. The `path` rule computes transitive closure — equivalent to BFS over all reachable nodes:

```lisp
; Base case: direct edge
(rule (path ?x ?y) (?x :edge ?y))
; Recursive case: follow chains
(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))

; All nodes reachable from node 0
(query db (find ?y) (where (path 0 ?y)))
; => all transitively reachable nodes
```

### OP_DFS — Depth-First Search

Explicit depth-first traversal from a source node, returning nodes in DFS visit order.

| Property | Value |
|---|---|
| Opcode | `OP_DFS (94)` |
| Complexity | O(V + E) within explored subgraph |
| Use case | Topological processing, cycle detection, graph exploration |

```c
ray_op_t* dfs_order = ray_dfs(g, src_node, rel, 10); /* max depth 10 */
```

### OP_RANDOM_WALK — Random Walk

Performs random walks from source nodes, selecting a uniformly random neighbor at each step. Used for node2vec-style embeddings and sampling.

| Property | Value |
|---|---|
| Opcode | `OP_RANDOM_WALK (98)` |
| Complexity | O(L) per walk, where L = walk_length |
| Use case | Graph sampling, node2vec, DeepWalk embeddings |

```c
ray_op_t* walk = ray_random_walk(g, src_node, rel, 80); /* 80-step walk */
```

## Shortest Path

### OP_SHORTEST_PATH — BFS Shortest Path

Finds the shortest unweighted path between two nodes using bidirectional BFS.

| Property | Value |
|---|---|
| Opcode | `OP_SHORTEST_PATH (82)` |
| Complexity | O(V + E) |
| Use case | Hop distance, unweighted routing, social distance |

```c
ray_op_t* path = ray_shortest_path(g, src, dst, rel, 20); /* max depth 20 */
```

#### Rayfall

Datalog reachability rules find all paths between two nodes. Pin both source and destination to check connectivity:

```lisp
; Reuse the path rule from above
(rule (path ?x ?y) (?x :edge ?y))
(rule (path ?x ?z) (?x :edge ?y) (path ?y ?z))

; Is node 4 reachable from node 0?
(query db (find ?y) (where (path 0 ?y)))
; => includes 4 if reachable
```

!!! note "Note"
    Datalog computes reachability (whether a path exists), not the actual shortest hop sequence. For weighted shortest paths or explicit path reconstruction, use the C API (`ray_shortest_path`, `ray_dijkstra`).

### OP_DIJKSTRA — Weighted Shortest Path

Dijkstra's algorithm for weighted shortest paths. Reads edge weights from a property column on the relationship.

| Property | Value |
|---|---|
| Opcode | `OP_DIJKSTRA (86)` |
| Complexity | O((V + E) log V) with binary heap |
| Use case | Weighted routing, cost-optimal paths, network flow |

```c
ray_op_t* path = ray_dijkstra(g, src, dst, rel,
    "weight",  /* edge weight column name */
    255        /* max depth */
);
```

### OP_ASTAR — A* Shortest Path

A* search with a coordinate-based heuristic (Haversine distance). Requires node property columns for latitude and longitude.

| Property | Value |
|---|---|
| Opcode | `OP_ASTAR (95)` |
| Complexity | O((V + E) log V), typically faster than Dijkstra with a good heuristic |
| Use case | Geospatial routing, map navigation, spatial networks |

```c
ray_op_t* path = ray_astar(g, src, dst, rel,
    "distance",   /* weight column */
    "lat",         /* latitude column */
    "lon",         /* longitude column */
    node_props,    /* node property table with lat/lon */
    255            /* max depth */
);
```

### OP_K_SHORTEST — Yen's k-Shortest Paths

Finds the k shortest paths between two nodes using Yen's algorithm. Each successive path is the shortest path that differs from all previously found paths.

| Property | Value |
|---|---|
| Opcode | `OP_K_SHORTEST (96)` |
| Complexity | O(kV(V + E) log V) |
| Use case | Route alternatives, network resilience, diverse path discovery |

```c
ray_op_t* paths = ray_k_shortest(g, src, dst, rel,
    "weight",  /* edge weight column */
    5          /* k = 5 shortest paths */
);
```

## Centrality

### OP_PAGERANK — PageRank

Iterative PageRank computation. Converges after `max_iter` iterations or when residuals fall below an internal threshold.

| Property | Value |
|---|---|
| Opcode | `OP_PAGERANK (84)` |
| Complexity | O(I * (V + E)), where I = iterations |
| Use case | Node importance ranking, influence analysis, link analysis |

```c
ray_op_t* pr = ray_pagerank(g, rel,
    100,   /* max iterations */
    0.85   /* damping factor */
);
```

!!! note "C API only"
    PageRank is an iterative numerical algorithm that operates directly on CSR arrays. It is not available as a Rayfall builtin. Use the [C API](#centrality) shown above.

### OP_DEGREE_CENT — Degree Centrality

Computes degree centrality for all nodes (normalized degree count).

| Property | Value |
|---|---|
| Opcode | `OP_DEGREE_CENT (92)` |
| Complexity | O(V) |
| Use case | Hub identification, connectivity analysis |

```c
ray_op_t* dc = ray_degree_cent(g, rel);
```

### OP_BETWEENNESS — Betweenness Centrality (Brandes)

Brandes' algorithm for betweenness centrality. Supports approximate computation via sampling to reduce cost on large graphs.

| Property | Value |
|---|---|
| Opcode | `OP_BETWEENNESS (99)` |
| Complexity | O(VE) exact, O(SE) sampled (S = sample_size) |
| Use case | Bridge detection, information flow bottlenecks |

```c
ray_op_t* bc = ray_betweenness(g, rel, 0);   /* 0 = exact (all nodes) */
ray_op_t* bc_approx = ray_betweenness(g, rel, 100); /* sample 100 sources */
```

### OP_CLOSENESS — Closeness Centrality

Closeness centrality measures how close a node is to all other reachable nodes. Supports sampling for large graphs.

| Property | Value |
|---|---|
| Opcode | `OP_CLOSENESS (100)` |
| Complexity | O(VE) exact, O(SE) sampled |
| Use case | Network accessibility, facility placement |

```c
ray_op_t* cc = ray_closeness(g, rel, 0); /* exact */
```

## Community Detection

### OP_LOUVAIN — Louvain Community Detection

Modularity-based community detection using the Louvain method. Iteratively merges communities to maximize modularity.

| Property | Value |
|---|---|
| Opcode | `OP_LOUVAIN (87)` |
| Complexity | O(V + E) per iteration, typically converges in a few passes |
| Use case | Community discovery, social clusters, network partitioning |

```c
ray_op_t* communities = ray_louvain(g, rel, 50); /* max 50 iterations */
```

### OP_CONNECTED_COMP — Connected Components

Finds connected components using label propagation. Each node receives a component ID.

| Property | Value |
|---|---|
| Opcode | `OP_CONNECTED_COMP (85)` |
| Complexity | O(V + E) |
| Use case | Graph partitioning, isolated subgraph detection, data lineage |

```c
ray_op_t* comp = ray_connected_comp(g, rel);
```

### OP_CLUSTER_COEFF — Clustering Coefficients

Computes the local clustering coefficient for each node: the fraction of its neighbors that are also connected to each other.

| Property | Value |
|---|---|
| Opcode | `OP_CLUSTER_COEFF (97)` |
| Complexity | O(V * d^2) where d = average degree |
| Use case | Network density, small-world analysis, triadic closure |

```c
ray_op_t* cc = ray_cluster_coeff(g, rel);
```

## Worst-Case Optimal Joins

### OP_WCO_JOIN — Leapfrog TrieJoin

Worst-case optimal join using Leapfrog TrieJoin (LFTJ). Finds triangles, k-cliques, and arbitrary pattern matches in the graph without materializing intermediate cross-products. Requires sorted adjacency lists in the CSR.

| Property | Value |
|---|---|
| Opcode | `OP_WCO_JOIN (83)` |
| Complexity | O(E^{3/2}) for triangle listing (worst-case optimal) |
| Use case | Triangle counting, k-clique enumeration, pattern matching, motif detection |

```c
/* Find all triangles: nodes (a,b,c) where a->b, b->c, a->c */
ray_rel_t* rels[] = {rel, rel, rel};
ray_op_t* triangles = ray_wco_join(g,
    rels,   /* 3 relationships (can be same or different) */
    3,      /* n_rels */
    3       /* n_vars (a, b, c) */
);
```

!!! note "Sorted CSR required"
    LFTJ relies on binary search within sorted adjacency lists. Always build relationships with `sort_targets = true` when using `OP_WCO_JOIN`.

## Spanning Trees

### OP_MST — Minimum Spanning Tree (Kruskal)

Computes the minimum spanning forest using Kruskal's algorithm with union-find. Returns the MST edges as a table.

| Property | Value |
|---|---|
| Opcode | `OP_MST (101)` |
| Complexity | O(E log E) |
| Use case | Network backbone, minimal wiring, clustering via MST cuts |

```c
ray_op_t* mst = ray_mst(g, rel, "weight");
```

## Vector Similarity

Vector similarity and nearest-neighbour search are documented separately, in
[Vector Search](vector-search.md). They are exposed as Rayfall builtins —
`.emb.cos-dist` / `.emb.l2-dist` / `.emb.inner-prod` / `norm` for direct distances, `.emb.knn` for
brute-force top-K, and `.emb.hnsw-build` / `.emb.ann` for HNSW-accelerated approximate
search — and as the `select … nearest:` clause for filter-aware retrieval.
Internally the filtered top-K paths lower to `OP_ANN_RERANK (102)` and
`OP_KNN_RERANK (103)`.

## Ordering

### OP_TOPSORT — Topological Sort (Kahn's)

Produces a topological ordering of a directed acyclic graph using Kahn's algorithm. Returns an error if cycles are detected.

| Property | Value |
|---|---|
| Opcode | `OP_TOPSORT (93)` |
| Complexity | O(V + E) |
| Use case | Task scheduling, dependency resolution, build systems |

```c
ray_op_t* order = ray_topsort(g, rel);
```

## SIP Optimization {#sip}

**Sideways Information Passing (SIP)** is an optimizer pass that propagates selection bitmaps (`RAY_SEL`) backward through `OP_EXPAND` chains. When a filter is applied after a graph expansion, SIP pushes the filter condition back to the source side of the expansion, allowing the executor to skip entire source nodes whose neighbors would all be filtered out.

This optimization is automatic. The optimizer detects EXPAND chains and propagates `sip_sel` bitmaps into the graph operation's extended node. During execution, the EXPAND opcode checks each source node against the SIP bitmap and skips it entirely if no neighbors can pass the downstream filter.

```c
/* Without SIP: expand all 1M source nodes, then filter */
/* With SIP: skip source nodes that can't produce passing results */

ray_op_t* nbrs   = ray_expand(g, src_nodes, rel, 0);
ray_op_t* pred   = ray_lt(g, nbrs, ray_const_i64(g, 1000));
ray_op_t* result = ray_filter(g, nbrs, pred);
/* Optimizer automatically injects SIP bitmap on the EXPAND node */
```

## Factorized Execution

Multi-hop graph expansions can produce enormous intermediate results (the cross-product of all paths). Rayforce avoids materializing these cross-products using **factorized vectors** (`ray_fvec_t`) and **factorized tables** (`ray_ftable_t`).

```c
/* Factorized vector: represents a column without materializing all rows */
typedef struct ray_fvec {
    ray_t*    vec;            /* underlying ray_t vector           */
    int64_t  cur_idx;        /* >= 0: single value at index       */
                             /* -1: full vector active            */
    int64_t  cardinality;    /* how many rows this represents     */
} ray_fvec_t;

/* Factorized table: accumulation buffer for WCO joins */
typedef struct ray_ftable {
    ray_fvec_t*  columns;     /* array of factorized vectors       */
    uint16_t    n_cols;
    int64_t     n_tuples;    /* factorized tuple count            */
    ray_t*       semijoin;    /* RAY_SEL bitmap of qualifying keys */
} ray_ftable_t;
```

When `factorized = 1` is set on a graph op's extended node, the executor emits factorized output instead of flat vectors. The factorized representation keeps each expansion level as a separate vector with a cardinality multiplier, deferring materialization until the final result is needed (via `ray_ftable_materialize`).

## Algorithm Summary

| Category | Opcode | Algorithm | Complexity |
|---|---|---|---|
| Traversal | `OP_EXPAND` | 1-hop CSR lookup | O(d) |
| Traversal | `OP_VAR_EXPAND` | BFS/DFS variable-length | O(V+E) |
| Traversal | `OP_DFS` | Depth-first search | O(V+E) |
| Traversal | `OP_RANDOM_WALK` | Random walk | O(L) |
| Shortest path | `OP_SHORTEST_PATH` | BFS | O(V+E) |
| Shortest path | `OP_DIJKSTRA` | Dijkstra (binary heap) | O((V+E) log V) |
| Shortest path | `OP_ASTAR` | A* with Haversine | O((V+E) log V) |
| Shortest path | `OP_K_SHORTEST` | Yen's algorithm | O(kV(V+E) log V) |
| Centrality | `OP_PAGERANK` | Iterative PageRank | O(I(V+E)) |
| Centrality | `OP_DEGREE_CENT` | Degree centrality | O(V) |
| Centrality | `OP_BETWEENNESS` | Brandes | O(VE) |
| Centrality | `OP_CLOSENESS` | Closeness centrality | O(VE) |
| Community | `OP_LOUVAIN` | Louvain modularity | O(V+E) |
| Community | `OP_CONNECTED_COMP` | Label propagation | O(V+E) |
| Community | `OP_CLUSTER_COEFF` | Local clustering | O(V*d^2) |
| Optimal join | `OP_WCO_JOIN` | Leapfrog TrieJoin | O(E^{3/2}) |
| Spanning | `OP_MST` | Kruskal | O(E log E) |
| Similarity | `OP_ANN_RERANK` | Filter-aware HNSW top-K (see [Vector Search](vector-search.md)) | O(D log N) |
| Similarity | `OP_KNN_RERANK` | Filter-aware brute-force top-K (see [Vector Search](vector-search.md)) | O(ND + N log K) |
| Ordering | `OP_TOPSORT` | Kahn's topological sort | O(V+E) |
