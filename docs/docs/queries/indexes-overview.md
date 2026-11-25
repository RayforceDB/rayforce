# Indexes Overview

A map of every index-like structure Rayforce ships — per-column accelerators, vector ANN indexes, linked columns, partition pruning, and graph indices. One mental model, then a decision matrix that points you at the right tool.

## What an "index" means in Rayforce

An **index** in Rayforce is a precomputed, optional structure that *rides alongside* the data it's built for. It is not a separate database object: it lives on the column or table it indexes, survives copy / refcount semantics, and travels with the data through the query pipeline. Whether queries actually *consult* that structure varies by kind — HNSW, linked columns, partition pruning, CSR, and the four `.idx.*` accelerators are all read by their respective query paths. See the [status section](#whats-wired-today) below.

Three properties hold for every kind of index documented on this page:

- **Opt-in.** Indexes are built explicitly when you decide the build cost is worth it. The system never builds one behind your back.
- **Mutation-aware.** Mutating the underlying data drops or invalidates the index by design — a stale index is a wrong-answer bug, so the runtime refuses to keep one. Rebuild after a write.
- **Transient by default.** Per-column accelerators live in memory only; HNSW handles can be persisted to disk explicitly. The on-disk file format for ordinary tables never carries an index. After loading, rebuild whatever you want indexed.

## The five index-like structures

### 1. Per-column accelerators — `.idx.zone` / `.idx.hash` / `.idx.sort` / `.idx.bloom`

Attach one of four kinds to a numeric vector (`.idx.hash` also to a symbol, string or guid vector). Each kind builds a structure suited to a different query shape: hash for equality lookups, sort for binary search and ordered access, zone for column-level min/max/null pruning, bloom for cheap probabilistic membership rejection. All four occupy the same per-column slot — one kind at a time today.

**Today's status:** all four are *built, inspectable via `(.idx.info)`, and consulted by the executor at six routing sites* — filter comparisons, filter IN, ORDER BY (single ascending key), distinct, find, and equi-joins (`inner-join`, `semi-mask`, `anti-mask`).  The routing is transparent: a query against an indexed column uses the fast path; the same query without the index falls back to the linear scan and returns identical results.

**Surface:** `(.idx.zone v)`, `(.idx.hash v)`, `(.idx.sort v)`, `(.idx.bloom v)`, `(.idx.drop v)`, `(.idx.has? v)`, `(.idx.info v)`. Numeric/boolean columns (`RAY_BOOL` through `RAY_TIMESTAMP`) are supported by all kinds; `.idx.hash` additionally accepts `RAY_SYM`, `RAY_STR` and `RAY_GUID`. `RAY_SYM`, `RAY_STR` and `RAY_GUID` on the non-hash kinds are deferred.

**See:** [Accelerator Indexes (reference)](indexes.md) · [Indexes Guide: choosing a kind](../guides/indexes.md#choose).

### 2. Vector ANN index — HNSW

Hierarchical Navigable Small World multi-layer proximity graph for approximate nearest neighbor search over float embedding vectors. Three distance metrics — cosine, L2, inner product. Built once with `.emb.hnsw.build`, queried with `.emb.ann`, optionally persisted to a directory with `.emb.hnsw.set` / `.emb.hnsw.get`.

**Surface:** `(.emb.hnsw.build col [metric] [M] [ef_c])`, `(.emb.ann handle query k [ef_search])`, `(.emb.knn col query k [metric])`, `(.emb.hnsw.set dir handle)`, `(.emb.hnsw.get dir)`, `(.emb.hnsw.free handle)`, `(.emb.hnsw.info handle)`. Brute-force `.emb.knn` needs no index and exists alongside.

**See:** [Vector Search & HNSW](../graph/vector-search.md) · [Indexes Guide: ANN workflow](../guides/indexes.md#workflow-ann).

### 3. Linked columns

A column whose values are row-id references into another table. Functions as a row-level index: dereferencing follows the link and resolves the target row at query time, similar in spirit to a foreign-key relationship but maintained at the column level.

**Surface:** `(.col.link col target-table)`, `(.col.unlink col)`, `(.col.link? col)`, `(.col.target col)`.

**Parted-table interaction:** a parted fact can carry a linked column targeting a non-parted dim (in-memory or splayed); per-segment `HAS_LINK` is preserved through `ray_read_parted` and segment streaming. Targets with any parted column are rejected at attach time. See [Linked Columns: Parted-Table Interaction](links.md#parted-table-interaction).

**See:** [Linked Columns](links.md).

### 4. Partition pruning

A storage layout, not a column-level index, but it functions as a coarse zone-map at the table level: the partition discriminator (date, integer, or symbol) selects whole sub-tables to load. Filters that target the partition column let the optimizer skip entire partitions before any scan begins.

**Surface:** implicit — the directory layout under your database root drives partition selection. The C API loader (`ray_part_load`) infers the partition type (`date` / `int` / `sym`) from the directory names.

**See:** [Columnar Storage](../storage/index.md) · [Storage Guide: partitioned tables](../guides/storage.md#partitioned-tables) · [Block Offloading](../architecture/offloading.md).

### 5. CSR graph index

A double-indexed Compressed Sparse Row adjacency structure (forward + reverse) attached to graph relationships. Used transparently by every graph opcode — `OP_EXPAND`, `OP_VAR_EXPAND`, `OP_SHORTEST_PATH`, `OP_WCO_JOIN` — and by Leapfrog Triejoin for worst-case optimal joins.

**Surface:** none directly — the CSR is built when a relationship is loaded and consulted automatically by graph queries. There is no `(.csr.*)` Rayfall surface today.

**See:** [Graph Storage](../graph/storage.md) · [Graph Algorithms](../graph/algorithms.md).

## Pick the right kind

Match the shape of your query to the structure that fits it.

| Want to… | Structure | Active today? |
|---|---|---|
| Skip whole columns where a predicate constant lies outside the value range | `.idx.zone` — min/max plus null count | Yes — O(1) all/none short-circuit at the filter site (int + float) |
| Make repeated `=` / `in` / `find` over a numeric, symbol, string or guid column O(matches) instead of O(n) | `.idx.hash` — chained open-addressing table | Yes — hash probe at filter EQ, filter IN, and find sites (symbol, string and guid columns included) |
| Binary-search a numeric column for range predicates or reuse the permutation for ORDER BY / distinct | `.idx.sort` — ascending row-id permutation | Yes — filter range (EQ/LT/LE/GT/GE), single-key ASC ORDER BY, distinct |
| Cheaply reject "definitely not in this set" probes for integer EQ filters | `.idx.bloom` — m-bit probabilistic filter | Yes — definite-absent proof at the filter EQ site (integer-family only) |
| Find the k nearest neighbors of an embedding vector by cosine, L2, or inner product | HNSW — `(.emb.hnsw.build)` + `(.emb.ann)` | Yes — `(.emb.ann)` consults the index |
| Resolve a cross-table reference at query time without a materialized join | Linked column — `(.col.link)` | Yes — column dereference resolves through the link |
| Skip whole sub-tables in a parted dataset based on the partition discriminator | Partition pruning — date / int / sym partitioning | Yes — optimizer pass rewrites filters |
| Traverse a graph — BFS, shortest path, betweenness, MST | CSR — transparent under graph opcodes | Yes — every graph opcode reads CSR directly |

## What's wired today

Rayforce is honest about phasing. The structures above all *build* correctly; integration depth varies by kind.

- **Per-column accelerators** — build kernels, the `(.idx.*)` surface, and executor routing are all shipped and tested. The executor consults indexes at six sites (filter comparisons, filter IN, ORDER BY, distinct, find); every fast path falls back to the scan on any miss and returns identical results. See [Routing Table](indexes.md#routing-table) for the full matrix and eligibility conditions. Set `RAY_IDX_STATS=1` to observe consult/hit counts at exit.
- **HNSW** — fully wired. `(.emb.ann)` consults the index immediately.
- **Linked columns** — fully wired. Dereference resolves through the link at query time.
- **Partition pruning** — the optimizer's pruning pass skips partitions whose discriminator falls outside a filter predicate's range. See [Pipeline & Optimizer](../architecture/pipeline.md).
- **CSR** — fully wired and used by every graph opcode.

## Where to go next

- [**Indexes Guide**](../guides/indexes.md) — procedural walk-through: when to build, how to choose, common workflows, lifecycle gotchas.
- [**Accelerator Indexes**](indexes.md) — full reference for the `.idx.*` family, including the routing table, eligibility conditions, and observability.
- [**Vector Search & HNSW**](../graph/vector-search.md) — ANN reference and worked examples.
- [**Linked Columns**](links.md) — cross-table reference reference.
- [**Columnar Storage**](../storage/index.md) — partitioning and on-disk layout.
