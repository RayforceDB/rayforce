<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="docs/logo-light.svg">
    <source media="(prefers-color-scheme: light)" srcset="docs/logo-dark.svg">
    <img src="docs/logo-dark.svg" alt="Rayforce" width="360">
  </picture>
</p>

<p align="center">
  Columnar analytics and graph traversal in one fused pipeline.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
  <a href="include/rayforce.h"><img src="https://img.shields.io/badge/header-rayforce.h-informational" alt="Single Header"></a>
  <a href="https://rayforcedb.github.io/rayforce2/"><img src="https://img.shields.io/badge/docs-website-e9a033" alt="Docs"></a>
  <img src="https://img.shields.io/badge/lang-C17-blue" alt="C17">
  <img src="https://img.shields.io/badge/deps-zero-brightgreen" alt="Zero Dependencies">
  <img src="https://img.shields.io/badge/allocator-custom-purple" alt="Custom Allocator">
  <a href="https://github.com/RayforceDB/rayforce2"><img src="https://img.shields.io/github/stars/RayforceDB/rayforce2?style=social" alt="GitHub Stars"></a>
</p>

---

Rayforce is a pure C17 zero-dependency embeddable engine where columnar
analytics and graph traversals share a single operation DAG, pass through a
multi-pass optimizer, and execute as fused morsel-driven bytecode. No malloc.

## Quick Start

```bash
make            # debug build (ASan + UBSan)
make release    # optimized build
make test       # run full test suite
./rayforce      # start the Rayfall REPL
```

## Rayfall REPL

Rayforce ships with **Rayfall** — a Lisp-like query language with a rich set
of builtins. The REPL prompt is `‣`:

<!-- Verified output from: ./rayforce /tmp/readme_test.rfl -->
```
‣ (set t (table [Symbol Side Qty]
    (list [AAPL GOOG MSFT AAPL GOOG]
          [Buy Sell Buy Sell Buy]
          [100 200 150 300 250])))

‣ (select {from:t by: Symbol Qty: (sum Qty)})
┌────────┬────────────────────────────┐
│ Symbol │            Qty             │
│  sym   │            i64             │
├────────┼────────────────────────────┤
│ AAPL   │ 400                        │
│ GOOG   │ 450                        │
│ MSFT   │ 150                        │
├────────┴────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘

‣ (pivot t 'Symbol 'Side 'Qty sum)
┌────────┬─────┬──────────────────────┐
│ Symbol │ Buy │         Sell         │
│  sym   │ i64 │         i64          │
├────────┼─────┼──────────────────────┤
│ AAPL   │ 100 │ 300                  │
│ GOOG   │ 250 │ 200                  │
│ MSFT   │ 150 │ 0                    │
├────────┴─────┴──────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown)│
└─────────────────────────────────────┘
```

## C API

Headers: [`include/rayforce.h`](include/rayforce.h) (types, memory, atoms,
vectors, tables, symbols), `src/ops/ops.h` (DAG construction, opcodes,
optimizer, executor, graph algorithms), `src/mem/heap.h` (allocator lifecycle).

<!-- Verified: compiles with cc -Iinclude -Isrc -->
```c
#include <rayforce.h>
#include "mem/heap.h"
#include "ops/ops.h"

int main(void) {
    ray_heap_init();
    ray_sym_init();

    /* Build a table */
    int64_t regions[] = {0, 0, 1, 1, 2, 2};
    int64_t amounts[] = {100, 200, 150, 300, 175, 225};
    ray_t* reg = ray_vec_from_raw(RAY_I64, regions, 6);
    ray_t* amt = ray_vec_from_raw(RAY_I64, amounts, 6);
    ray_t* tbl = ray_table_new(2);
    tbl = ray_table_add_col(tbl, ray_sym_intern("region", 6), reg);
    tbl = ray_table_add_col(tbl, ray_sym_intern("amount", 6), amt);
    ray_release(reg); ray_release(amt);

    /* Group by region, sum amounts */
    ray_graph_t* g = ray_graph_new(tbl);
    ray_op_t* keys[]    = { ray_scan(g, "region") };
    uint16_t  agg_ops[] = { OP_SUM };
    ray_op_t* agg_ins[] = { ray_scan(g, "amount") };
    ray_op_t* grp = ray_group(g, keys, 1, agg_ops, agg_ins, 1);

    ray_t* result = ray_execute(g, grp);

    if (result && !RAY_IS_ERR(result)) ray_release(result);
    ray_graph_free(g);
    ray_release(tbl);
    ray_sym_destroy();
    ray_heap_destroy();
}
```

## Capabilities

|                              | Rayforce | DuckDB | Polars |
|------------------------------|:--------:|:------:|:------:|
| Native graph engine (CSR)    |    ✓     |        |        |
| Graph algorithms             |    ✓     |        |        |
| Worst-case optimal joins     |    ✓     |        |        |
| Factorized execution         |    ✓     |        |        |
| SIP optimizer                |    ✓     |        |        |
| Embeddable (single header)   |    ✓     |        |        |
| Zero external dependencies   |    ✓     |        |        |
| Built-in query language      |    ✓     |        |        |
| Fused morsel pipelines       |    ✓     |   ✓    |   ✓    |
| Multi-pass query optimizer   |    ✓     |   ✓    |        |
| COW ref counting             |    ✓     |        |   ✓    |
| Custom memory allocator      |    ✓     |   ✓    |        |
| Window functions & ASOF join |    ✓     |   ✓    |   ✓    |

## How It Works

**Build** — Construct a lazy DAG: scans, filters, joins, aggregations, window
functions, graph traversals. Nothing executes yet.

**Optimize** — Multi-pass rewriting: type inference → constant folding → SIP →
factorize → predicate pushdown → filter reorder → projection pushdown →
partition pruning → fusion → DCE.

**Execute** — Fused morsel-driven bytecode processes 1024-element chunks that
stay L1-resident. Radix-partitioned hash joins size partitions to fit L2.
Thread pool dispatches morsels in parallel.

## Features

**Execution engine**
- Lazy operation DAG — nothing runs until `ray_execute`
- Multi-pass optimizer with sideways information passing
- Fused morsel-driven bytecode — element-wise ops merged into single-pass chunks
- Radix-partitioned hash joins sized for L2 cache
- Thread pool with parallel morsel dispatch

**Graph engine**
- Double-indexed CSR storage (forward + reverse), mmap support
- BFS, DFS, Dijkstra, A*, PageRank, Louvain, Betweenness, LFTJ, and more
- Factorized execution avoids materializing cross-products
- SIP propagates selection bitmaps backward through expand chains

**Rayfall language**
- Arithmetic, string, aggregation, joins, higher-order, I/O builtins
- Lambdas compile lazily to bytecode, run in computed-goto VM
- `select`/`update`/`pivot` bridge to the DAG optimizer at runtime

**Memory**
- Buddy allocator with slab cache — O(1) for ~90% of allocations
- Thread-local arenas, lock-free allocation, COW ref counting
- No system allocator — `ray_alloc`/`ray_free` for everything

**Vector search**
- Multi-metric HNSW index (cosine / L2 / inner-product) with save/load
- Rayfall builtins: `cos-dist` / `l2-dist` / `inner-prod` / `norm` / `knn`
  and the HNSW lifecycle `hnsw-build` / `ann` / `hnsw-save` / `hnsw-load` /
  `hnsw-free` / `hnsw-info`
- Filter-aware ANN via `select ... where ... nearest (ann handle query) take k`
- Iterative streaming scan: the `where` predicate is pushed into HNSW's
  beam loop so rejected candidates don't consume result slots

**Storage**
- Columnar files with mmap, splayed tables, date-partitioned tables
- CSV reader with parallel mmap parse, type inference, null handling

## Project Structure

```
include/rayforce.h         Single public header
src/mem/                    Buddy allocator, slab cache, arena, COW
src/core/                   Type system, platform abstraction, runtime
src/vec/                    Vector, list, string, selection bitmap ops
src/table/                  Table, symbol intern table
src/store/                  Column files, CSR, splayed/parted tables, HNSW
src/ops/                    DAG, optimizer, fused executor, LFTJ
src/io/                     CSV reader/writer (parallel mmap)
src/lang/                   Rayfall parser, evaluator, bytecode VM
src/app/                    REPL, terminal, pretty-printer
test/                       Test suites
examples/rfl/               Rayfall example scripts
examples/                   C API examples
website/                    Documentation site (GitHub Pages)
```

## Documentation

Full docs: **[rayforcedb.github.io/rayforce2](https://rayforcedb.github.io/rayforce2/)**

- [Quick Start](https://rayforcedb.github.io/rayforce2/docs/quick-start.html) — build, REPL, first query
- [Rayfall Language](https://rayforcedb.github.io/rayforce2/docs/rayfall-syntax.html) — syntax and builtins
- [Data Types](https://rayforcedb.github.io/rayforce2/docs/data-types.html) — types and collections
- [Queries](https://rayforcedb.github.io/rayforce2/docs/queries-select.html) — select, joins, pivot, window
- [C API](https://rayforcedb.github.io/rayforce2/docs/c-api-core.html) — full API reference
- [Graph Engine](https://rayforcedb.github.io/rayforce2/docs/graph-algorithms.html) — algorithms
- [Architecture](https://rayforcedb.github.io/rayforce2/docs/architecture-pipeline.html) — DAG, optimizer, memory

## License

[MIT](LICENSE)
