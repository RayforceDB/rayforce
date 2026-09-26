# Rayforce

Embeddable columnar analytics and graph traversal engine in pure C.

Rayforce combines morsel-driven vectorized execution, a multi-pass query optimizer, and a native CSR graph engine in a single pipeline. It is queried through the **Rayfall** language, exposes a C API for embedding, and runs on Linux and macOS, and builds on Windows from source (MSYS2 CLANG64).

[Quick Start](getting-started/quick-start.md){ .md-button .md-button--primary }
[Functions Reference](language/functions.md){ .md-button }

---

## At a glance

- **Pure C, zero external dependencies.** Single binary, single shared library, single header.
- **Columnar with morsels.** Vectorized execution over fixed-size morsels; SIMD where possible.
- **Multi-pass optimizer.** Predicate pushdown, join reordering, accelerator-index selection.
- **Graph engine.** Native CSR storage, PageRank, Dijkstra, betweenness, Louvain, MST.
- **Datalog.** Recursive rules over EAV triples.
- **Rayfall language.** Lisp-flavoured functional surface; full table-and-graph combinator vocabulary.
- **IPC.** Client/server with auth, restricted mode, and connection-lifecycle hooks.

## Where to go next

- New to Rayforce? Start with the [Quick Start](getting-started/quick-start.md) — build, REPL, first query.
- Want the language reference? See [Rayfall Syntax](language/syntax.md) and [Functions Reference](language/functions.md).
- Looking for a specific builtin? Each top-level namespace has its own page under [Namespaces](namespaces/index.md): `.time.*`, `.ipc.*`, `.sys.*`, `.os.*`, `.csv.*`, `.db.*`, `.idx.*`, `.graph.*`, `.log.*`, `.col.*`, `.repl.*`.
- Embedding into a C application? See the [C API](c-api/core.md).
