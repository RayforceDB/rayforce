# Parquet completion plan

Status: in progress, 2026-09-30. No third-party implementation or runtime
libraries. An external reference SQL engine is a development oracle only. Existing local work
and the measured full ClickBench splayed baseline are preserved.

Current checkpoint: **P1's full-size evidence gates are complete**: all 43
direct queries and all 12 partitioned load cases verified. P3's ClickBench
correctness gate is also complete. Final failure-path, fuzz, scalar and tooling
validation passed. The broader capabilities listed under P2–P6 remain
partial; see the explicit open list at the end.

## P1: Establish the remaining end-to-end evidence

- Extend the load harness to CSV and Parquet parted output. Compare every
  cell with the retained, verified full CSV splayed reference, resolving each
  output's symbol dictionary independently and respecting partition row order.
- Measure full 99,997,497-row loads at 8 and 28 workers, three repetitions,
  including indexes, reopen and durable writes. Retain raw timings, resource
  counters, schema, binary identity and correctness results.
- Run all 43 ClickBench query shapes against direct Parquet. Record individual
  failures rather than stopping at the first unsupported query. Record any
  necessary translations explicitly, including raw Unix temporal units.
- Check results against the reference SQL engine, accounting for SQL's unspecified tie ordering
  and unordered LIMIT. Do not mark execution-only results as verified.
- Fix correctness failures before treating timings as performance evidence.

Completion requires full-size parted load evidence and an explicit outcome for
every query: verified, incorrect, unsupported, timeout or resource failure.
The full direct-query correctness gate requires 43 verified queries; recording
failures is useful progress, not completion of that gate.

## P2: Bound memory and improve import scaling

- Profile dictionary insertion, dictionary flushing, decode, native indexes
  and durable output separately at 1, 8 and 28 workers.
- Add an overall scratch-memory budget and admission/backpressure for row-group
  work; account for page buffers, dictionaries and materialized results.
- Reduce shared symbol-domain work without changing symbol identity or row
  order. Preserve batched insertion and block allocation.
- Build native index summaries during decoding, or reuse Parquet summaries
  only where types, null semantics and index granularity are compatible.
  Recompute when metadata is absent, untrusted or too coarse.
- Remove the additional splayed STR write pass where possible; handle the
  native 32-bit string-pool limit explicitly.

Acceptance: all-cell equivalence, bounded scratch under a configured budget,
and repeated full-load improvement against P1 and the existing splayed baseline.
Separate physical/logical CPU scaling and storage/memory limits in the report.

## P3: Complete direct-query execution for the benchmark

- Support explicit source type overrides, including raw temporal units and
  dictionary-backed text, without materializing the entire file up front.
- Add string/dictionary predicates, safe additional predicate combinations,
  and filter-first/output-later decoding. Measure bytes and pages avoided.
- Stream grouped aggregates and distinct with memory accounting; add spilling
  before claiming bounded-memory execution for arbitrary cardinality or sorts.
- Broaden table-operation integration beyond select where a lazy source has
  well-defined semantics. Preserve conservative fallback for dynamic expressions.

Acceptance: all 43 full ClickBench queries verified; compare query latency,
peak memory and decoded bytes with the P1 record. Page-index/Bloom speedups
need representative indexed inputs because the ClickBench file has neither.

## P4: Make import layouts operationally useful

- Configurable rows per partition and date/key partitioning for CSV and Parquet.
- Checkpoint/resume with source identity and completed-part validation.
- Define append and schema-evolution behavior, then implement it with explicit
  compatibility checks and recoverable publication.
- Test interruption, failed writes, restart and destination collision handling.

Acceptance: deterministic row coverage/order for sequential partitions;
correct key routing for keyed partitions; resumed output equivalent to a fresh
import; no publication of incomplete output.

## P5: Expand format coverage and fidelity

- Native implementations of additional codecs and delta/byte-stream-split
  encodings, prioritized by concrete input files. Each codec is a separate
  reviewed change with malformed-input and cross-producer fixtures.
- Explicit mappings for decimal, fixed byte arrays, UINT64 and remaining
  logical types; design nested LIST/MAP/STRUCT representation before decoding.
- Validate UTF-8 and define timezone-annotation retention.
- Resolve native null/value collisions before claiming lossless conversion.
  This requires a storage/type-model decision, not a silent reader workaround.
- Add multi-file datasets, schema reconciliation and partition discovery;
  remote sources follow a defined range-read and caching interface.

Acceptance: an interoperability matrix, exact supported-value round trips or
documented rejection, and no added third-party library dependencies.

## P6: Harden and document

- Fuzz footer/page/encoding/index parsers and run sustained corruption tests.
- Exercise cancellation and allocation/write failures across parallel paths.
- Validate supported operating systems and architectures, including scalar
  decoder fallbacks. Run sanitizers and focused leak checks.
- Publish reproducible load/query commands, raw measurements, supported
  features and remaining limits. No performance claim without a matching run.

## Evidence already complete

- Full ClickBench CSV versus Parquet splayed import, 105 columns, three runs
  at both 8 and 28 workers; every cell checked.
- Results: `bench/parquet_load/results/2026-09-30-full.json`.
- Final previous implementation passed 3,971 sanitizer tests.

## Work log

- 2026-09-30: P1 started. Existing load harness only supports splayed; query
  oracle covers 12 shapes. Full input and verified CSV reference remain local.
- 2026-09-30, reboot recovery: recovered the interrupted session from its local
  log. Source changes and the full splayed measurement report survived; the
  input, CSV reference, sample query results and in-flight parted run under
  `/tmp` did not. The current harness now handles parted all-cell verification
  and all 43 direct-query shapes, including tie-aware oracle checks.
- Recovery work and raw logs now live under `build/parquet-recovery/` (ignored
  by git, persistent across reboot). Restoring the exact Athena input from the
  URL and SHA-256 recorded in `bench/parquet_load/results/2026-09-30-full.json`.
- Fixed repeated dictionary persistence: private Parquet imports flush the
  shared domain after workers join; fresh CSV parted imports stage the root,
  decode directly into its domain and flush once before rename. Existing CSV
  roots keep flush-before-column behavior. Bulk CSV output still requires
  external fsync for durability, as performed by the load harness.
- Added regressions for independently reopened symbols across parallel groups,
  failed/stale CSV staging, trailing-slash destinations and existing-root
  symbol replacement. ASan/UBSan: 3,974/3,974 passed. After final staging sync
  and path checks: CSV 71/71 and Parquet 14/14 passed. Release build passed.
- Restored the full input: 14,779,976,446 bytes; SHA-256 matches
  `a390f6cb782f6aaef278c72fc1dd86c4f30bc843ebab3c159e9bd4d45ddb079f`.
  The recovery driver is now `build/parquet-recovery/continue.py`, with an atomic
  `status.json` checkpoint and a log per phase. It runs a 1M-row query census,
  reconstructs and verifies the full CSV splayed reference, measures all-cell
  checked parted loads at 8/28 workers (three repetitions per format), then
  runs the full 43-query census. Queries have a recorded 48-GiB address-space
  ceiling and 180-second full-input timeout; resource failures remain failures.
  The fixed release binary and its SHA-256 are retained alongside the logs.
  Recovered full-size P1 evidence is pending; previous timing reports remain
  historical evidence and P2–P6 are not marked complete.
- Recovered query discrepancies: duplicate tied rows now get an oracle
  multiplicity check; Q29's Rayfall translation matches the SQL regexp's
  newline/protocol/nonempty-host behavior; computed `by: {alias: expression}`
  keys compile through the query DAG so `if` chooses a branch per row.
  The latter also fixes ordinary in-memory queries. Regressions cover nested
  conditions, prefiltering and references to earlier computed aliases.
- Final recovery validation: ASan/UBSan **3,975/3,975 passed**; the one-million-row
  ClickBench sample has **43/43 queries verified** against the reference SQL engine, including
  exact full-row membership and multiplicity for tied alternatives. Report:
  `bench/parquet_load/results/2026-09-30-sample-queries.json`. Separate verifier
  self-checks reject invented rows, wrong ranks and excess duplicate rows.
- The restored 99,997,497-row CSV splayed reference was checked against a full
  Parquet import across all 105 columns. One recovered CSV parted case
  overlapped diagnostics and was interrupted during external fsync; it is
  excluded from performance evidence. The continuation checks its data, then
  runs fresh measurements in `build/parquet-recovery/parted-final/` and the
  full query census in `build/parquet-recovery/full-queries-final/`. Their
  status is recorded in `build/parquet-recovery/status.json`; full-input
  completion has not yet been claimed. Both benchmark harnesses now publish
  their per-case result checkpoints by atomic replacement.
- The recovered CSV parted output passed every-cell comparison:
  **99,997,497 rows × 105 columns, 1,526 partitions**. Fresh `parted-final`
  measurements are running; the corrected full-input query census is queued
  after them, without concurrent tests or diagnostic queries.
- Full-size direct-query baseline: **30/43 verified**. Nine queries hit the
  4 GiB STR pool limit; four exhausted the recorded 48 GiB address-space
  allowance. Explicit `types` on reads/scans, backed by private refcounted
  symbol domains, raised this to **39/43**. Results are preserved in
  `bench/parquet_load/results/2026-09-30-full-query-baseline.json` and
  `2026-09-30-full-query-typed.json`.
- Projection now recognizes arithmetic, conditionals and safe local bindings;
  it falls back when a user lambda shadows a builtin. Inclusive integer
  `within` predicates and standalone `like` filter before materialization.
  Text is interned only after surviving a reader filter. The remaining four
  full queries passed on the next build. Their focused report is
  `2026-09-30-full-query-pushdown-four.json`; this combined evidence is not yet
  the single-build 43-query gate. Private dictionaries now also use the
  existing chunked derived-key evaluator, avoiding persistent intermediate
  runtime symbols.
- First fresh partitioned pair at eight workers passed every-cell comparison:
  CSV **303.638 s**, Parquet **815.970 s** (one run each, not medians). Repeated
  per-column directory syncs were removed from private Parquet writes. Files
  and directories are synced after workers join, before publication. A matched
  65,536-row diagnostic and a three-repeat 8/28-worker comparison at a
  262,144-row cap are queued on the frozen build. Row-group boundaries still
  terminate Parquet partitions; reports record this difference.
- Native import options now accept `rows` (1..1,048,576), `types`, and `strict`.
  Strict mode rejects decoded nonnull values that collide with native nulls
  and malformed annotated UTF-8, without changing the default storage model.
  Reader/scan options accept `strict` too. Regressions cover real nulls,
  sentinel widening, both plain/dictionary paths, invalid UTF-8 classes,
  custom partition sizes and refusal to publish a failed strict import.
- The current evidence driver is `build/parquet-recovery/finish-evidence.py`;
  its atomic `progress.json` supersedes the older recovery status for ongoing
  work. It gates on sanitizers, requires all 43 full queries to verify, then
  runs the load measurements serially. The pinned release is
  `build/parquet-recovery/rayforce-completion`.
- Added a native libFuzzer target (`fuzz/fuzz_parquet.c`) for footer, page,
  encoding and optional index parsing, seeded with the small format fixtures.
  Its sustained run is pending after performance measurements.
- Frozen build `3efe1db444730918af49610cb26a313729f54bc3ccc3f15fb61173f84ae0b86b`
  passed **3,976/3,976 ASan/UBSan tests** and **43/43 full-input queries** with
  the same 48 GiB address-space ceiling. The complete report is
  `bench/parquet_load/results/2026-09-30-full-queries.json`. Q29 dropped from
  47.399 s / 38,091,648 KiB RSS to 10.085 s / 10,847,900 KiB RSS in the focused
  and final runs, respectively, after enabling chunked expression evaluation
  over the private dictionary. These are individual diagnostic timings.
- Write-failure review found success-path `fclose` results ignored by the
  generic native column writer, including nested/list/table cases. Added error
  propagation before rename and a Linux `RLIMIT_FSIZE` regression asserting
  that both bulk and durable saves preserve an existing destination on a
  buffered flush failure. Validation is queued after timings; this error-path
  hardening is newer than the frozen benchmark binary.
- Matched 65,536-row Parquet diagnostic completed and passed all-cell
  verification: **231.194 s** durable total, **146.540 s** process, versus the
  earlier single run's 815.970 s / 730.660 s. OS page cache was managed normally;
  this is a diagnostic comparison, not a repeated median. The 12-case
  CSV/Parquet comparison at 8/28 workers and a 262,144-row cap is running.
- Added durable run manifests and `--resume` to both development harnesses.
  They reject changed builds/settings/sources, preserve completed cases and
  archive interrupted outputs before restarting a case. This does not add
  native-import checkpoint/resume. A real small-input integration check is
  queued after the timed runs, fuzzing and final sanitizer/scalar checks.
  Copies of the harness actually used by the running load measurements are
  retained in each work directory as `harness-used.py`; the resume additions
  were made after those processes started.
- Added exclusive publication for new CSV and Parquet destinations: a final
  rename cannot replace a concurrently created empty directory. Linux/macOS
  use their no-replacement rename flags; Windows omits replacement permission.
  Unsupported hosts/filesystems fail with staging retained. The destination
  collision regression is queued with the final store checks. This hardening
  also follows the frozen performance build.

- The 12-case partitioned comparison completed: every output passed comparison
  of all **10,499,737,185 cells** and native types. Durable medians at eight
  workers are CSV **241.077 s**, Parquet **118.807 s** (**2.03×**); at 28 workers,
  CSV **233.398 s**, Parquet **117.488 s** (**1.99×**). The cap is 262,144 rows;
  CSV creates 382 partitions and Parquet 501 because row-group boundaries also
  terminate partitions. Report: `2026-09-30-parted-full.json`. The matched-cap
  diagnostic is `2026-09-30-parted-sync-diagnostic.json`.
- Final ASan/UBSan suite: **3,978/3,978 passed**. The same Parquet suite with
  the SIMD translation unit compiled for its scalar fallback: **14/14 passed**.
  Both native write-failure and exclusive-publication regressions pass.
  An end-to-end 64-byte file-size-limit check reproduces incomplete publication
  on the preserved old binary; the fixed binary returns an I/O error, retains
  staging, and does not publish the destination.
- Real small-input restart integration checks pass for both harnesses,
  including rejection of changed source/settings and retention of completed
  cases. The native verifier rejects altered numeric data and dictionary text.
  The final optimized rebuild also passes all-cell checks for both splayed
  and partitioned imports of 1,000 rows × 105 columns. A dedicated `.parquet`
  namespace reference is linked from the documentation navigation.

- Sustained libFuzzer validation completed **9,600,493 inputs in 601 seconds**
  with no ASan/UBSan/LSan findings; peak fuzzer RSS was 1,053 MiB. The successful
  run used `setarch -R`, following the existing sanitizer/ASLR workaround.
  Clang needed this host's GCC 11 C++ library search path for the development
  fuzzer; the final product still links only libc, libm and the system loader.
  The optimized binary also rejects the injected write failure before
  publication. Raw validation details are recorded in
  `bench/parquet_load/results/2026-09-30-validation.json`.

Still open: a reader-wide memory budget/admission policy; arbitrary-cardinality
group/sort spilling; key/date partitions and crash-resumable imports; append and
schema evolution; additional concrete codecs/encodings, nested/multifile/remote
formats; broader platform and failure-injection validation. Passing ClickBench
does not imply these capabilities or completion of P2–P6.
