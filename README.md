<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/logo-light.svg">
    <source media="(prefers-color-scheme: light)" srcset="assets/logo-dark.svg">
    <img src="assets/logo-dark.svg" alt="Rayforce" width="360">
  </picture>
</p>

<p align="center">
  Columnar analytics and graph traversal in one fused pipeline.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue.svg" alt="MIT License"></a>
  <a href="include/rayforce.h"><img src="https://img.shields.io/badge/header-rayforce.h-informational" alt="Single Header"></a>
  <a href="https://rayforcedb.github.io/rayforce/"><img src="https://img.shields.io/badge/docs-website-e9a033" alt="Docs"></a>
  <img src="https://img.shields.io/badge/lang-C-blue" alt="C">
  <img src="https://img.shields.io/badge/deps-zero-brightgreen" alt="Zero Dependencies">
  <img src="https://img.shields.io/badge/allocator-custom-purple" alt="Custom Allocator">
  <a href="https://github.com/RayforceDB/rayforce"><img src="https://img.shields.io/github/stars/RayforceDB/rayforce?style=social" alt="GitHub Stars"></a>
</p>

---

Rayforce is a pure C zero-dependency embeddable engine where columnar
analytics and graph traversals share a single operation DAG, pass through a
multi-pass optimizer, and execute as morsel-driven bytecode compiled at
execution time. No malloc.

## Install

**Homebrew** (macOS & Linux):

```bash
brew install rayforcedb/tap/rayforce
```

**Debian / Ubuntu** (`.deb`, x86-64) — grab the `.deb` from the
[latest release](https://github.com/RayforceDB/rayforce/releases/latest):

```bash
curl -LO https://github.com/RayforceDB/rayforce/releases/download/vX.Y.Z/rayforce_X.Y.Z_amd64.deb
sudo dpkg -i rayforce_X.Y.Z_amd64.deb
```

**Prebuilt tarball** (Linux x86-64 / macOS arm64) is also attached to each
release. Or build [from source](#quick-start) below — zero dependencies, just
`make`.

## Quick Start

```bash
make            # debug build (ASan + UBSan)
make release    # optimized build
make test       # run full test suite
./rayforce      # start the Rayfall REPL
```

`make` compiles with `clang` when `CC` is not set; `make CC=gcc` (or `CC=gcc`
in the environment) builds with gcc.

## Rayfall REPL

Rayforce ships with **Rayfall** — a Lisp-like query language with a rich set
of builtins. The REPL prompt is `‣`:

<!-- REPL session.  Borders ASCII-rendered so GitHub's monospace
     stack renders glyph widths consistently — Unicode box-drawing
     chars (┌─┬┐│└┴┘) lose subpixel-precise widths in some fonts
     and the table looks "twisted".  The data is what `./rayforce`
     actually returns; only the border glyphs differ. -->
```
‣ (set t (table [Symbol Side Qty]
    (list [AAPL GOOG MSFT AAPL GOOG]
          [Buy Sell Buy Sell Buy]
          [100 200 150 300 250])))

‣ (select {from:t by: Symbol Qty: (sum Qty)})
+--------+----------------------------+
| Symbol |            Qty             |
|  SYM   |            I64             |
+--------+----------------------------+
| AAPL   | 400                        |
| GOOG   | 450                        |
| MSFT   | 150                        |
+-------------------------------------+
| 3 rows (3 shown) 2 columns (2 shown)|
+-------------------------------------+

‣ (pivot t 'Symbol 'Side 'Qty sum)
+--------+-----+----------------------+
| Symbol | Buy |         Sell         |
|  SYM   | I64 |         I64          |
+--------+-----+----------------------+
| AAPL   | 100 | 300                  |
| GOOG   | 250 | 200                  |
| MSFT   | 150 | 0Nl                  |
+-------------------------------------+
| 3 rows (3 shown) 3 columns (3 shown)|
+-------------------------------------+
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

## Embedding

`make lib` builds the engine as a static library, `librayforce.a`, with no
dependencies beyond libc, libm and pthreads. The build compiles library
objects with `-ffunction-sections -fdata-sections` (the Makefile's
`LIB_CFLAGS` flavour, applied automatically to the `.lib.o` objects that go
into the archive), so no extra flags are needed at this step.

To discard everything your code never reaches, pass `-Wl,--gc-sections` to
the linker on the final link, and `-s` to strip the result. A DAG-route
client, meaning one that builds and executes query plans through the
`ray_graph_*`, `ray_scan` and `ray_execute` C API and never calls
`ray_eval_str` or loads `.rfl` files, never touches the Rayfall parser,
compiler, REPL or IPC server, and section GC drops that whole surface
(mixing the two routes pulls the parser back in):

```sh
make clean && make lib
cc -O2 -Iinclude -Isrc examples/analytics.c librayforce.a \
   -lm -lpthread -Wl,--gc-sections -s -o analytics
```

The command above is the Linux one. On macOS, use `-Wl,-dead_strip` in
place of `-Wl,--gc-sections` (the Makefile's platform block does the same
for its own release links). On MinGW, link the winsock libraries as well:
`-lws2_32 -lmswsock -lkernel32 -ladvapi32`, with the same section-GC flag
the toolchain accepts.

### Staged headers (`make install-headers`)

`make install-headers` copies the headers a C client needs into a staging
root, so a tree staged from one machine can be carried to a build host
that has no checkout. The staged set is `include/rayforce.h` (public)
plus the internals that bindings include directly - `src/ops/ops.h`,
`src/io/parquet.h`, `src/mem/heap.h` - and the two those internals reach
through their own quoted includes, `src/store/hnsw.h` (from `ops.h`) and
`src/core/platform.h` (from `heap.h`). They are copies rather than
symlinks, staged read-only, mirroring the source layout: the internals
land under `install/include/rayforce/`, and `rayforce.h` is staged
twice - inside that tree and at the stage root
(`install/include/rayforce.h`), which is the copy that lets
`-Iinstall/include` resolve `<rayforce.h>`. `DESTDIR` and
`INSTALL_INCLUDE` move the stage root, and `make clean` removes what the
same overrides staged.

The staged layout needs **two include roots**: the internals include
their siblings by quoted, `src/`-relative path, so `-Iinstall/include`
alone does not resolve `"ops/ops.h"`:

```sh
cc -O2 -Iinstall/include -Iinstall/include/rayforce/src \
   examples/analytics.c librayforce.a -lm -lpthread -o analytics
```

The target fails loudly: any failed copy aborts it, so a run that
returns success has left a complete tree, and one that cannot stage says
so rather than reporting success over a partial one.

Staging is a convenience for building against one engine revision, not
an SDK contract. What it promises is the header contents at the commit
that staged them - no ABI stability, and no API stability either. The
staged set carries internal types and layouts that can change between
revisions, so stage from the same commit you built `librayforce.a`
from.

A checkout at the current commit links `analytics.c` to a 3.47 MB
stripped binary, from a 10.22 MB archive; sizes move with the tree, so
this is just one measurement, not a bound.

Memory follows consumed-argument conventions documented per function in
the headers: arguments marked `consumes` are released by the call, and
returns marked `borrowed` stay owned by the container. On error paths the
library releases the references it holds. Values are COW, managed with
`ray_retain`/`ray_release`; error objects are owned and freed with
`ray_error_free`.

## How It Works

**Build** — Construct a lazy DAG: scans, filters, joins, aggregations, window
functions, graph traversals. Nothing executes yet.

**Optimize** — Multi-pass rewriting: type inference → constant folding → idiom
rewrite → SIP → factorize → predicate pushdown → filter reorder → projection
pushdown → partition pruning → DCE.

**Execute** — Element-wise subtrees are compiled to flat bytecode at execution
time and evaluated over 1024-element morsels that stay L1-resident, with no
intermediate heap allocation. Radix-partitioned hash joins size partitions to
fit L2. Thread pool dispatches morsels in parallel.

## Features

**Execution engine**
- Lazy operation DAG — nothing runs until `ray_execute`
- Multi-pass optimizer with sideways information passing
- Expression compilation at execution time — element-wise subtrees compiled to flat bytecode, evaluated in 1024-row morsel batches with no intermediate materialization
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

Full docs: **[rayforcedb.github.io/rayforce](https://rayforcedb.github.io/rayforce/)**

- [Quick Start](https://rayforcedb.github.io/rayforce/docs/quick-start.html) — build, REPL, first query
- [Rayfall Language](https://rayforcedb.github.io/rayforce/docs/rayfall-syntax.html) — syntax and builtins
- [Data Types](https://rayforcedb.github.io/rayforce/docs/data-types.html) — types and collections
- [Queries](https://rayforcedb.github.io/rayforce/docs/queries-select.html) — select, joins, pivot, window
- [C API](https://rayforcedb.github.io/rayforce/docs/c-api-core.html) — full API reference
- [Graph Engine](https://rayforcedb.github.io/rayforce/docs/graph-algorithms.html) — algorithms
- [Architecture](https://rayforcedb.github.io/rayforce/docs/architecture-pipeline.html) — DAG, optimizer, memory

## Python bindings

Rayforce has Python bindings at **[rayforce-py](https://github.com/RayforceDB/rayforce-py)** — contributions welcome.

## Contributing

Contributions are welcome. You can help by:

- Reporting bugs and requesting features via [GitHub Issues](https://github.com/RayforceDB/rayforce/issues)
- Submitting pull requests
- Creating example scripts and use cases
- Improving documentation

## Development Partnership

Rayforce is jointly developed with and sponsored by **[Lynx](https://www.lynxtrading.com/)**.

This partnership has been instrumental in making Rayforce a mature,
production-ready engine. Lynx's active involvement in development and their
commitment to innovative open-source technologies in the financial sector has
enabled Rayforce to reach its full potential.

## License

[MIT](LICENSE)
