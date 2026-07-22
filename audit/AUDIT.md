# AUDIT — Rayforce (C engine, Rayfall runtime, tests, and documentation)

Operator language: English.

## 0. Scope and batching rules

The audit covers authored, tracked project material: `include/`, `src/`, `test/`,
`fuzz/`, `examples/`, `bench/`, `docs/`, `website/`, `overrides/`, `scripts/`,
`packaging/`, `.github/`, and the root build/governance files. Each tracked file
is an addressable unit. A heading or enclosing-symbol range is an addressable
sub-unit when a Markdown or C file is too large for one attentive pass.

Queue globs are resolved with `git ls-files` at the beginning of a pass; the
resolved file list and any symbol/section split must be recorded in the pass
report. Brace sets such as `{arena,cow}` are compact notation: expand every
listed member into its own pathspec. Related implementation/header pairs and
small tests may be batched.
Large units such as `src/ops/query.c`, `src/ops/group.c`, `test/test_exec.c`, and
`test/test_lang.c` must be split by enclosing symbols if the charter cannot be
completed attentively. Any unfinished range becomes a new queued pass under
XCHECK.md §4 rule 4.

Excluded: `.git/`, `.worktrees/`, `.agents/`, `.codex/`, `audit/` itself,
untracked/generated objects and binaries (`*.o`, `*.d`, `*.a`, `rayforce*`),
coverage/fuzz corpora, release output, caches, and vendored font/image binary
content. Generated/static site output is checked only for contracts that the
repository intentionally maintains (redirects, navigation, and brand shell),
not for minified or third-party asset internals.

Inventory baseline (2026-07-22): 169 tracked C/header files under `src/`, one
770-line public header, 72 C tests plus four test headers, 432 Rayfall tests,
98 authored files under `docs/docs/`, eight fuzz source/policy files, and ten
GitHub policy/workflow files. Counts are planning estimates, not pass evidence.

## 1. Norms catalog

| id | norm source | scope |
|---|---|---|
| N01 | `include/rayforce.h` | Public types, ownership/lifetime, error, vector, table, progress, and API contracts. |
| N02 | `src/**/*.h`, especially `src/ops/ops.h`, `src/core/runtime.h`, `src/mem/*.h`, and `src/store/*.h` | Internal module interfaces and invariants; use only the header relevant to the audited implementation. |
| N03 | `docs/docs/c-api/*.md`, `docs/docs/architecture/*.md`, `docs/docs/language/*.md`, `docs/docs/reference/all-functions.md`, `docs/docs/storage/*.md`, and `docs/docs/guides/{errors,memory}.md` | Published behavior, architecture, language, storage/wire, error/null, and lifecycle contracts. |
| N04 | `README.md` | Product capabilities, supported workflows, project structure, examples, and performance/architecture promises. |
| N05 | `test/*.c`, `test/rfl/**/*.rfl`, and `test/test.h` | Executable behavior and regression contracts. A test is a norm only for behavior it asserts explicitly. |
| N06 | `CONTRIBUTING.md` and `Makefile` | C17, zero-dependency, warnings-as-errors, sanitizer, TSan, fuzz, test-discovery, and build-flavor policy. |
| N07 | `.clang-tidy` and `.tsan-suppressions` | Correctness-only static-analysis policy and the requirement that race suppressions be justified. |
| N08 | `fuzz/README.md`, fuzz drivers, seed policy, and `Makefile` fuzz targets | Untrusted-input surfaces, sandbox restrictions, fuzz coverage, and crash-regression procedure. |
| N09 | `RELEASE.md`, `packaging/*`, `.github/workflows/release.yml`, and `.github/release-notes.sh` | Version, release, artifact, packaging, and supported-platform contracts. |
| N10 | `mkdocs.yml`, `docs/index.md`, and the tracked redirect/static shell files | Documentation navigation, published paths, exclusions, redirects, and site structure. |
| N11 | `.github/workflows/{ci,nightly,version-guard,retarget-prs,pages,rayforce-audit}.yml` | Automated branch, CI, stability, documentation, and audit gates. |
| N12 | ISO C17 and the platform APIs selected by `Makefile`/`src/core/platform.h` | Language-level undefined behavior, integer/pointer rules, atomics, and declared POSIX/Windows portability. Use an exact clause or platform API contract in findings. |
| N13 | Internal consistency | For contradictions not governed elsewhere, quote both conflicting passages as the norm and evidence. |

## 2. Dimensions

| key | what it catches | norm source |
|---|---|---|
| memory-lifetime | leaks, double release/free, use-after-free, invalid COW mutation, arena/heap ownership errors, overflow and out-of-bounds access | N01, relevant N02 header, N03 memory docs, N06 sanitizer policy, N12 |
| concurrency-portability | data races, broken atomics, unsafe cross-thread ownership, backend divergence, unsupported platform assumptions | N02, N03 architecture docs, N06, N07, N11, N12 |
| language-semantics | parser/compiler/evaluator/builtin behavior inconsistent with syntax, null/error rules, function reference, or executable tests | N03, N05, N13 |
| query-correctness | optimizer rewrites or execution paths that change results, mishandle nulls/types, or violate DAG/parallel execution contracts | N02, N03 pipeline/DAG docs, N05, N13 |
| storage-integrity | corrupt or incompatible serialization, journal/AOF replay errors, unsafe file handling, symbol-domain loss, persistence/IPC contract drift | N01, N02, N03 storage/IPC docs, N05, N08 |
| api-doc-consistency | declarations, ownership, errors, examples, and registered builtins that disagree across public headers, docs, implementation, or tests | N01, N02, N03, N04, N05, N13 |
| resilience-security | untrusted parser/decoder/input failures, sandbox escapes, restricted-mode gaps, crash/DoS paths, missing fuzz regression coverage | N06, N08, N11, N12 |
| build-release | debug/release/hardened semantic drift, missing source coverage, non-reproducible versioning, broken CI/release/packaging/platform promises | N04, N06, N09, N11, N12 |
| test-adequacy | normative behavior with no meaningful assertion, tests that cannot detect the promised failure, or suites omitted from discovery/gates | N03, N05, N06, N08, N11 |
| docs-structure | broken navigation/redirects, orphaned or duplicated reference material, stale project structure, or internal contradictions | N04, N10, N13 |
| performance-contract | optimizations that violate result semantics or documented resource/complexity claims; benchmark scenarios that do not measure their stated path | N03 architecture docs, N04, `bench/**`, N13 |

Stylistic preferences are not an audit dimension: `.clang-tidy` explicitly
turns style churn off. A style-only observation has no project norm and is at
most `info` under XCHECK.md §6.

## 3. Unit map

| unit family | tracked units / size estimate | audit use |
|---|---:|---|
| Root contracts | `README.md`, `CONTRIBUTING.md`, `RELEASE.md`, `Makefile`, `.clang-tidy`, `.tsan-suppressions`, `mkdocs.yml` (7 files, ~1.1k lines plus 407-line Makefile) | Product, build, analysis, release, and documentation norms. |
| Public API | `include/rayforce.h` (1 file, 770 lines) | Public ABI/API, ownership, errors, and types. |
| Application | `src/app/*` (5 files, ~4.3k lines) | CLI lifecycle, REPL, terminal, formatting. |
| Core/platform | `src/core/*` (34 files, ~7.4k lines) | Runtime, types, pool, progress, IPC/socket, platform backends, diagnostics. |
| I/O | `src/io/*` (2 files, ~3.4k lines) | CSV parsing/writing and parallel I/O. |
| Language | `src/lang/*` (15 files, ~9.5k lines) | Parser, compiler, evaluator/VM, environments, special commands. |
| Memory | `src/mem/*` (8 files, ~3.6k lines) | Arena, buddy heap, system allocation, COW/refcounts. |
| Operations | `src/ops/*` (67 files, ~95.3k lines) | DAGs, optimizer, execution, relational/graph/vector operations and builtins. |
| Storage | `src/store/*` (20 files, ~8.0k lines) | AOF/journal, serde, column/partition stores, CSR, HNSW, metadata. |
| Tables/symbols | `src/table/*` (8 files, ~3.8k lines) | Tables, dictionaries, symbol domains/interning. |
| Values/vectors | `src/vec/*` (10 files, ~2.6k lines) | Atoms, vectors, lists, strings, selections, embedding contract. |
| C test harness | `test/*.c`, `test/*.h` (76 files, ~125k lines) | Executable contracts and adequacy checks; the largest files require symbol splits. |
| Rayfall tests | `test/rfl/**/*.rfl` (432 small files, ~72.9k lines in 37 subject directories) | Language/output regression contracts; batch by subject directory. |
| Fuzzing | `fuzz/*.c`, `fuzz/common.h`, `fuzz/README.md`, committed dictionaries/seeds (43 tracked units; drivers/policy ~425 lines) | Parser, decoder, evaluator, CSV, and journal adversarial coverage. |
| Examples | `examples/*.c`, `examples/rfl/**/*.rfl` (25 files, ~1.3k lines) | User-facing API and language behavior. |
| Benchmarks | `bench/**` (14 tracked files, ~4.6k lines) | Performance claims and benchmark validity. |
| Documentation | `docs/docs/**/*.md`, docs root content/config support, `docs/llms.txt` (109 authored text/code units, ~19.1k lines; binary assets separate) | Published contracts, structure, links, and reference completeness. |
| Site shell | `overrides/**`, authored `website/*.{html,css,js}`, redirect stubs (about 9 principal authored units, ~2.7k lines) | Navigation, redirects, and brand shell consistency. |
| Automation/release | `.github/**` (10 files, ~883 lines), `scripts/**` (5 files, ~444 lines), `packaging/**` (2 files), root release scripts | CI/nightly/release/security gates and packaging. |

## 4. Pass queue

All passes stop when their resolved scope is exhausted, 15 findings are filed,
or context is exhausted. In the latter two cases the Auditor records explicit
coverage and queues the unresolved files/symbol ranges as a new pass.

- [x] P-01 — api-doc-consistency × public lifecycle: compare `include/rayforce.h`, `docs/docs/c-api/core.md`, and `test/test_public_api.c` for declarations, ownership, error, and lifecycle contracts.
- [ ] P-02 — memory-lifetime × arena/COW: audit `src/mem/{arena,cow}.{c,h}` in implementation/header pairs; split into two passes if needed.
- [ ] P-03 — memory-lifetime × heap/system allocator: audit `src/mem/{heap,sys}.{c,h}` against N01/N02/N03/N06/N12.
- [ ] P-04 — memory-lifetime × atoms/vectors: audit `src/vec/{atom,vec}.{c,h}` against public type, allocation, null, and refcount contracts.
- [ ] P-05 — memory-lifetime × list/string/selection: audit `src/vec/{list,str,sel}.{c,h}` plus `src/vec/embedding.h`; split by module.
- [ ] P-06 — memory-lifetime × tables/symbol domains: audit `src/table/{dict,domain,sym,table}.{c,h}`; split by implementation/header pair.
- [ ] P-07 — memory-lifetime × runtime/types: audit `src/core/{runtime,types}.{c,h}` against N01 and error/null/lifetime docs.
- [ ] P-08 — query-correctness × blocks/morsels: audit `src/core/{block,morsel}.{c,h}` against pipeline and execution contracts.
- [ ] P-09 — concurrency-portability × pool/progress/platform: audit `src/core/{pool,progress,platform}.{c,h}`; split by module and record backend assumptions.
- [ ] P-10 — concurrency-portability × event backends: compare `src/core/{poll,epoll,kqueue,iocp}.c` and relevant declarations for semantic parity.
- [ ] P-11 — resilience-security × IPC/socket: audit `src/core/{ipc,sock}.{c,h}` and the applicable IPC tests for framing, authentication/restriction, cleanup, and platform error paths.
- [ ] P-12 — resilience-security × diagnostics/timing: audit `src/core/{crash,qlog,qmeasure,timer}.c` and their headers (`qstats.h`, `profile.h`, etc.) for failure-path safety and hardened-build promises.
- [ ] P-13 — api-doc-consistency × CLI/REPL: audit `src/app/*` against README quick-start/REPL behavior and focused `test/test_{repl,term}.c`; split by module.
- [ ] P-14 — language-semantics × parser/numeric parser: audit `src/lang/{parse,compile}.c` only for parsing/compilation boundaries plus `src/core/numparse.{c,h}`; split large symbol ranges.
- [ ] P-15 — language-semantics × evaluator/VM: audit `src/lang/eval.{c,h}`, `src/lang/cal.h`, and `src/lang/internal.h` against syntax/error/null contracts; split `eval.c` by symbols.
- [ ] P-16 — language-semantics × environment/format/system commands: audit `src/lang/{env,format,nfo,syscmd}.{c,h}` against namespace, rendering, and restricted-mode norms.
- [ ] P-17 — storage-integrity × CSV: audit `src/io/csv.{c,h}`, `test/test_csv.c`, and `docs/docs/storage/index.md` CSV sections; split `csv.c` by symbols.
- [ ] P-18 — storage-integrity × serialization/file metadata: audit `src/store/{serde,fileio,meta}.{c,h}` against wire/storage docs and relevant tests.
- [ ] P-19 — storage-integrity × column/partition/splay: audit `src/store/{col,part,splay}.{c,h}` against storage layout, pruning, and round-trip contracts; split by module.
- [ ] P-20 — storage-integrity × AOF/journal: audit `src/store/{aof,journal}.{c,h}`, focused tests, and documented replay/integrity contracts.
- [ ] P-21 — storage-integrity × CSR/HNSW: audit `src/store/{csr,hnsw}.{c,h}` against graph/vector persistence contracts and focused tests; split by subsystem.
- [ ] P-22 — query-correctness × aggregation engine: audit `src/ops/agg.c`, `agg_engine.{c,h}`, `agg_stream.c`, `agg_acc.h`, and `agg_registry.h`; split by engine phase.
- [ ] P-23 — language-semantics × scalar operations: audit `src/ops/{arith,cmp,temporal,string,strop}.c` with corresponding reference sections and tests; split by operation family.
- [ ] P-24 — resilience-security × builtin/system surface: audit `src/ops/{builtins,system,glob}.c` for registration, error propagation, restricted mode, and fuzz sandbox contracts.
- [ ] P-25 — language-semantics × collections/vectors: audit `src/ops/{collection,fvec}.c` and `fvec.h` against collection/function reference and tests; split large files by symbols.
- [ ] P-26 — query-correctness × datalog/LFTJ: audit `src/ops/{datalog,lftj}.{c,h}` against published APIs and focused tests; split by subsystem.
- [ ] P-27 — storage-integrity × operation journal/dump: audit `src/ops/{journal,dump}.c` and `journal.h` against serialization/replay norms.
- [ ] P-28 — query-correctness × embedding/rerank: audit `src/ops/{embedding,rerank}.c` against vector-search docs and executable tests.
- [ ] P-29 — query-correctness × executor/expression VM: audit `src/ops/exec.{c,h}` and `src/ops/expr.c`; each large file gets its own symbol-range subpass.
- [ ] P-30 — query-correctness × filtering/fused paths: audit `src/ops/{filter,fused_pred,fused_topk}.c` and `fused_{group,pred,topk}.h`; split by path.
- [ ] P-31 — query-correctness × graph/traversal: audit `src/ops/{graph,graph_builtin,traverse}.c` and `graph.h` against graph docs/tests; split by algorithm families.
- [ ] P-32 — query-correctness × grouping/hash/HLL: audit `src/ops/group.c`, `hash.h`, and `hll.{c,h}`; `group.c` must be split by enclosing symbol ranges.
- [ ] P-33 — query-correctness × index/link operations: audit `src/ops/{idxop,linkop}.{c,h}` against index/link docs and tests.
- [ ] P-34 — query-correctness × joins: audit `src/ops/join.c` against join contracts and focused C/Rayfall tests; split by join strategy.
- [ ] P-35 — query-correctness × optimizer/planner: audit `src/ops/{opt,idiom,plan}.{c,h}` against the documented pass order and focused optimizer tests.
- [ ] P-36 — query-correctness × query engine: audit `src/ops/query.c` against query docs/tests; split into explicit select/update/partition/execution symbol ranges.
- [ ] P-37 — query-correctness × pipe/row selection: audit `src/ops/{pipe,rowsel}.{c,h}` against selection/null and pipeline contracts.
- [ ] P-38 — query-correctness × pivot/table operations: audit `src/ops/{pivot,tblop}.c` against table operation docs/tests.
- [ ] P-39 — query-correctness × sort/window: audit `src/ops/{sort,window}.c` against null/order/window contracts; split `sort.c` by symbols.
- [ ] P-40 — api-doc-consistency × DAG registry: compare `src/ops/{ops,internal}.h`, opcode/registration tables, `docs/docs/c-api/dag.md`, and `test/test_agg_registry.c`; split declaration families.
- [ ] P-41 — api-doc-consistency × complete builtin surface: compare the registered runtime surface with `docs/docs/reference/all-functions.md` and `docs/docs/language/functions.md`; record exact missing/extra/signature cases.
- [ ] P-42 — api-doc-consistency × language syntax: compare `docs/docs/language/{syntax,control-flow,math,string,repl}.md` with parser/evaluator behavior and focused Rayfall tests; split by document.
- [ ] P-43 — api-doc-consistency × query/architecture docs: compare `docs/docs/{queries,architecture}/**/*.md` with the applicable operation headers/tests; batch at most three small docs or one large doc.
- [ ] P-44 — api-doc-consistency × storage/IPC docs: compare `docs/docs/storage/*.md` and namespace docs for `.db`, `.csv`, `.ipc`, `.log` with headers/registrations/tests; split by namespace.
- [ ] P-45 — language-semantics × errors/nulls: compare `docs/docs/guides/errors.md`, public error/null declarations, and `test/rfl/{null,expr,regress}/**/*.rfl`; split by error then null behavior.
- [ ] P-46 — test-adequacy × memory/core C tests: audit `test/test_{arena,buddy,cow,heap,heap_parallel,block,morsel,pool,platform,progress,runtime,types}.c` against the corresponding contracts; batch related small tests.
- [ ] P-47 — test-adequacy × language/runtime C tests: audit `test/test_{compile,err,format,lang,repl,term,numparse}.c`; split `test_lang.c` by symbol ranges.
- [ ] P-48 — test-adequacy × execution/query C tests: audit `test/test_{exec,opt,pipe,rowsel,group_extra,group_pushdown,join_buildside,window,sort,idx_route,partition_exec}.c`; split large tests by subsystem.
- [ ] P-49 — test-adequacy × storage/I/O C tests: audit `test/test_{aof,csv,ipc,journal,meta,splay,store,public_api}.c` against persistence and API contracts.
- [ ] P-50 — test-adequacy × values/tables C tests: audit `test/test_{atom,dict,domain,f64_nullmodel,list,sel,str,sym,table,vec}.c` against N01/N02/N03.
- [ ] P-51 — test-adequacy × graph/advanced C tests: audit `test/test_{csr,datalog,embedding,graph,graph_builtin,index,lftj,link,traverse,fused_topk,fvec}.c`; split large algorithm tests.
- [ ] P-52 — test-adequacy × stress/harness: audit `test/{main,stress_eval,stress_store,test_stress_eval,test_stress_matrix,test_stress_random,test_audit}.c` and CI discovery/gating.
- [ ] P-53 — test-adequacy × Rayfall scalar/language: audit `test/rfl/{arith,cmp,lang,type,temporal,strop}/**/*.rfl` for meaningful assertions and reference coverage; split by directory.
- [ ] P-54 — test-adequacy × Rayfall collections/nulls: audit `test/rfl/{collection,dict,hof,null,ops,symbol,table}/**/*.rfl`; split by directory.
- [ ] P-55 — test-adequacy × Rayfall aggregation/query: audit `test/rfl/{agg,group,query,pivot}/**/*.rfl`; split by directory.
- [ ] P-56 — test-adequacy × Rayfall joins/order/windows: audit `test/rfl/{join,linkop,sort,window}/**/*.rfl`; split by directory.
- [ ] P-57 — test-adequacy × Rayfall persistence/system: audit `test/rfl/{io,journal,storage,store,system,mem}/**/*.rfl`; split by directory.
- [ ] P-58 — test-adequacy × Rayfall graph/Datalog/vector: audit `test/rfl/{graph,datalog,embedding}/**/*.rfl`; split by directory.
- [ ] P-59 — test-adequacy × integration/regress/optimizer: audit `test/rfl/{integration,regress,opt,fused,lazy}/**/*.rfl`; split integration by subject and record untested contracts.
- [ ] P-60 — resilience-security × fuzz surfaces: compare all six fuzz drivers, `fuzz/README.md`, seeds/dictionaries, and CI/Makefile discovery; one driver/surface per subpass.
- [ ] P-61 — build-release × build flavors/portability: audit `Makefile`, `CONTRIBUTING.md`, `.clang-tidy`, `.tsan-suppressions`, and `.github/workflows/{ci,nightly}.yml` for source coverage and semantic/gate parity.
- [ ] P-62 — build-release × version/release/packaging: audit `RELEASE.md`, release/version workflows and scripts, `packaging/*`, and install claims in README; split release and packaging paths.
- [ ] P-63 — api-doc-consistency × examples: compile/compare `examples/*.c` and `examples/rfl/**/*.rfl` against the referenced APIs and README/docs; batch at most three examples.
- [ ] P-64 — docs-structure × navigation/redirects: audit `mkdocs.yml`, docs indexes, `docs/llms.txt`, redirect stubs, and link targets for promised reachability and orphan/duplicate content.
- [ ] P-65 — docs-structure × site shells: compare `overrides/**`, authored `website/*.{html,css,js}`, docs root pages, and Pages workflow for route/navigation/brand consistency.
- [ ] P-66 — performance-contract × architecture/benchmarks: compare README and architecture performance/resource claims with `bench/**` scenario setup and the implementation path each benchmark invokes; split by benchmark directory.

## 5. Limits

max_findings_per_pass: 15
remediation_batch_size: 8
class_threshold: 3
reopen_limit: 2

No graph assistance was present during planning. Cross-unit relationships are
encoded directly in the API/doc, implementation/test, and build/workflow
passes above.
