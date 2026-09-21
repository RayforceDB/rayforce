# Grouping engine scaling plan

Status: implementation and local acceptance are complete at `f61a4eda`.
The feature is ready for PR review into `dev`; required CI checks gate merging.
This extends the type-coverage plan.

Acceptance compares immutable release binaries with identical generated inputs.
The rebased ASan/UBSan suite passes 3,828/3,828 tests. Measurements and the
commands that reproduce them are kept with the benchmark harness in
`bench/groupby_shapes/`, not in the published docs.

## Objective

Remove avoidable grouping, allocation, synchronization, and merge overhead for
all supported grouped aggregate families. Preserve types, nulls, ordering,
overflow behavior, symbol domains, and cancellation. This work covers grouping
and its query integration, not unrelated joins, CSV parsing, or sorting queries.

Equal latency across aggregates is not an acceptance criterion. Sum consumes
every valid value; extrema can avoid updates; exact median and distinct need
additional data structures. Each family needs its own baseline and scaling proof.

## Implementation and evidence

| Family / shape | Execution | Completed evidence |
|---|---|---|
| BOOL/U8/I16/I32/DATE/TIME min/max, eligible dense single key | Shared state through explicit concurrent-update capability | Native/null contracts; adversarial value order and dominant-key cases at every worker count |
| Numeric sums, count, average, statistics, product, boolean and binary reductions | Bounded partitioned or task-local dense states; native scalar output | Registry-wide writer contracts, independent arithmetic oracles and separate family timings |
| I64/TIMESTAMP/F32/F64 extrema | Partitioned dense | Native-width, null and non-finite contracts; repeated worker sweep |
| Selected and composite dense keys | Deduplicated native payload and shared source-row indices | Selected-row identity, empty selection and composite/mixed contracts; worker sweep |
| Sparse integer/temporal/SYM keys | Radix | Sparse/skew/null matrix; phase profiles and memory measurements |
| Float, STR, GUID and LIST keys | Shared parallel directory with complete equality and deterministic first-row IDs | Full-value/canonicalization oracles, race checks and repeated worker sweep |
| First/last, wide extrema, median, quantile, mode, top/bottom K | Shared stable row slices, row-balanced groups and dominant-group splitting | Independent order/rank/frequency oracles; 90 internal K runs; dominant-group worker sweep |
| Count-distinct | Pair-hash partitioning and parallel stable output ordering | Native/wide value and ordering oracles; repeated worker sweep |
| Mixed streaming/indexed aggregates and expressions | Shared groups and stable row slices | Independent mixed contracts and repeated worker sweep |

The benchmark harness in `bench/groupby_shapes/` holds the complete
900-configuration summary, all 2,700 process records, phase investigations and
regression repeats. Baseline failures receive no speedup claim. Measured cold
setup and bounded-memory tradeoffs remain explicit.

## Work-package status

| Package | Status | Evidence |
|---|---|---|
| G0 — Baseline and census | Complete | Immutable baseline/candidate hashes; 77 cases and 2,700 typed process comparisons; complete timings and peak RSS |
| G1 — Dense streaming | Complete | All streaming writers, unary/binary/mixed contracts, bounded state budget tests, dense route profiles and worker matrix |
| G2 — Shared key layouts | Complete | Selection/composite/sparse/wide-key oracles and full worker matrix; shared stable row indices |
| G3 — Ordered and distinct consumers | Complete | Dominant-group worker matrix, exact rank/order/frequency tests and 90 top/bottom-K histogram checks |
| G4 — Acceptance and delivery | Local acceptance complete; ready for PR review | ASan/UBSan 3,828/3,828, TSan 24/24 at four total threads, 2,700 synthetic runs, 90 kernel oracles, 130 regression repeats and 68 profiles; immutable release and privacy checks |

## Work packages, in dependency order

### G0 — Reproducible baseline and coverage census

1. Preserve release at bb81e621 and current release separately; record revisions
   and worker settings. Unit-test builds must not replace benchmark binaries.
2. Cover aggregate families with reproducible synthetic fixtures and typed
   result comparisons. Keep native temporal extrema as a control.
3. Measure temporal and integer sums, count, average, variance, wide extrema,
   binary reductions, and mixed queries independently. Record cold execution
   separately from five warm executions in each fresh process.
4. Add synthetic fixtures for native widths, nulls, uniform/clustered/skewed
   keys, low/high cardinality, sparse ranges, selection and composite keys.
5. Record actual route and fallback reason alongside phase profiles. Unsupported
   operation/type pairs remain explicit errors, not artificial fast-path goals.

Exit: a coverage ledger with baseline, current route, correctness oracle and
worker sweep for every family above. Complete: 77 synthetic cases, six worker
settings and three rounds; immutable binary hashes accompany the process records.

### G1 — Close the dense streaming refactor

1. Measure histogram, scatter, reduction, partial merge and output separately
   for sum/count/statistics, not just shared extrema.
2. Reduce duplicated input fields and state traffic where profiles justify it.
3. Tune partition tasks using work and state size, with bounded split storage;
   validate a dominant null key and a dominant non-null key.
4. Keep strategy choice capability-driven. Add shared updates only where safe
   and measured; contended atomic sums are not an automatic replacement.
5. Check default uses all physical cores (SMT siblings excluded, #606);
   distinguish pool size from bounded tasks.

Exit: correct unary, binary and mixed streaming results; repeated synthetic
8/default scaling measurements for min, sum, count and statistics. Investigate
flat scaling to the responsible phase before declaring this package complete.

### G2 — Share grouping across selections and key shapes

1. Profile selected/composite task-local replication and sparse radix traffic.
2. Extend partition ownership to selected/composite keys where a bounded plan
   wins, preserving stable row identity for ordered aggregates.
3. Reuse one group-ID/index layout across all aggregates in a query.
4. Parallelize wide-key grouping stages that remain serial; preserve complete
   string/GUID values, symbol domains and float canonicalization.

Exit: correctness and scaling matrix for selection, composite, sparse and wide
keys, including empty selection and all-null keys; no O(workers × rows) state.

### G3 — Ordered, buffered and distinct consumers

1. Isolate count-distinct regression before changing its implementation.
2. Schedule independent indexed groups by estimated rows/work, rather than
   equal group counts. Measure dominant-group behavior explicitly.
3. Use mergeable bounded state where the operation permits it; preserve stable
   first/last semantics and exact median/quantile/distinct results.
4. Bound peak memory including index layouts, scatter and retained values.
5. Test mixed streaming/indexed queries so one consumer does not rebuild groups
   or cause a hidden whole-query performance cliff.

Exit: family-specific speedup evidence, correctness oracles, and bounded-memory
checks. An algorithmic lower bound must be distinguished from avoidable serial
engine work with profiles; documentation alone does not close a measured defect.

### G4 — Acceptance and delivery

- Run saved baseline and candidate sequentially, alternating order, at
  1/2/4/8/16/default workers, at least three fresh processes per case.
- Report per-process cold time, warm median and range, plus peak RSS. Retain
  outliers; do not describe small noisy differences as scaling.
- Compare sorted typed results exactly for integer and ordered outputs. For
  floating reductions, compare keys/types/nulls exactly and values against an
  independent oracle with operation-specific absolute/relative tolerances;
  parallel reassociation may change low bits.
- Investigate repeatable regressions over 5%; accept neither noise-based
  speedup claims nor an unexplained slowdown as completion.
- Run complete ASan/UBSan suite and relevant TSan tests after functional edits.
- Review diff and commit the feature branch; rebuild release with committed
  revision and rerun synthetic min/sum controls on the delivered binary. Push
  `perf/grouping-engine-scaling` and open a PR into `dev` after acceptance.

Completion requires all packages above to have evidence and explicit status.
The current min(time) result and passing tests do not close the whole plan.

## Implemented changes

- Native-width payload scatter and shared input-field deduplication.
- Bounded dense state allocation, task-local initialization, and parallel output.
- Capability-driven shared extrema and row-balanced partition reduction.
- Shared parallel key directory with full float, string, GUID and LIST equality.
- Stable row slices reused by mixed streaming and indexed consumers.
- Row-weighted ordered/wide consumers and exact dominant-group rank selection.
- Row-split winner selection, bounded partial top/bottom-K heaps, and exact
  partitioned mode frequencies with bounded local preaggregation.
- Parallel native result gathering and adaptive-width symbol slice offsets.
- Bounded exact distinct preaggregation and parallel stable output ordering.
- Domain-aware symbol read views during immutable worker phases.
- Typed empty output columns and corrected wide distinct admission.

Full ASan/UBSan validation passes 3,828/3,828 tests; targeted TSan passes 24/24.
The complete synthetic acceptance and regression investigations are recorded in
the results report. Integration is gated by the required PR checks.

## Dominant-group execution stages

Assigning a whole group to one worker leaves a single large group serial even
when grouping itself scales. Consumer scheduling therefore considers rows
within groups as well as independent groups:

- First/last and wide extrema select candidates from contiguous row chunks and
  merge them in original index order. For extrema, null-only chunks contribute
  no candidate.
- Small top/bottom K uses one bounded heap per source chunk, then selects from
  their union. Larger candidate sets merge in parallel. Large K first uses
  exact three-way selection, then sorts only the retained K rows and merges
  their sorted chunks. Each merge is split by output rank, including the final
  pair. Two row-index buffers bound scratch storage by input rows rather than
  workers times K. Partially filled serial heaps are heapified before sorting.
- Mode locally combines exact value counts in a fixed-size table, partitions
  partial counts by full value hash, and reduces partitions independently.
  Equality checks resolve hash collisions. Counts carry the earliest original
  position, preserving ties even when source-row indices are reordered.
- Median and quantile use exact parallel rank selection within large groups.
  Splitting uses the common parallel grain and the per-worker row share, so a
  few medium-sized groups can also use the pool. Histogram oracles cover the
  grain boundary, medium groups and dominant groups.
- Native output gathering writes disjoint result payload ranges and publishes
  null metadata after workers finish.

All stages are query-local. No previously computed query answers are retained.
The synthetic matrix includes single-group first/last, symbol/string extrema,
numeric/string mode, and numeric/string/symbol top/bottom K. Independent tests
also cover temporal and narrow native widths, null-only and empty groups,
frequency ties, reordered row indices, and local frequency-table flushing.

The grouped query compiler currently accepts K from 1 through 1024. This change
preserves that language contract. `topk_consumer.c` separately exercises the
internal indexed kernel with K near the group size; `topk_scaling.py` checks
all output values against an independent histogram and records cold/warm times
and peak RSS. This prevents a query-front-end limit from hiding kernel gaps.

## Follow-up on low-worker overhead

- When task-local group counters fit in 256 KiB, build stable row slices with
  direct source-task histograms and prefix offsets. This avoids a second row
  buffer and partitioning passes while retaining source order. Larger group
  directories keep the bounded partitioned layout.
- Partition larger directories into contiguous group-id ranges so workers
  own adjacent output regions instead of scattering interleaved groups across
  the same pages. Use one source range per worker; split hot partitions by rows.
- Dispatch native top-K input types outside the scan loop. Discard losing rows
  before entering heap maintenance, and specialize native heap comparisons.
- Separate top-K scheduling from heap ownership: each worker retains one
  private heap across smaller source tasks, then sorts it once after the scan.
  Exact preselection handles larger K, followed by bounded sorting runs and
  merges split by output rank.
- Read native first/last null sentinels directly after type dispatch; retain
  periodic cancellation checks and source-row winner semantics.

- For task-local dense states, sample each source range before choosing eager
  initialization. Narrow local key ranges retain lazy initialization instead of
  initializing the entire global domain. Sampling controls work only.
- Use one dense scatter source range per worker to reduce adjacent writes;
  partition reduction still splits dominant partitions by their row counts.

These changes are included in the final synthetic sweep and sanitizer checks.

## Shared directory allocation

- Use 32-bit atomic representatives when row IDs fit, retaining 64-bit entries
  for larger dense inputs. The empty sentinel remains outside the valid row range.
- Allocate representative output storage after exact group counts are known.
- Initialize the directory in coarse aligned ranges to reduce concurrent first
  writes to the same huge pages. Pad partition counters between worker slices.

## Native validity and small-domain scheduling

- Native accumulator loops select the sentinel reader from the registered input
  representation at compile time. Nullable rows no longer repeat type dispatch.
- Use additional independent source tasks for small dense domains when their
  replicated state fits within one eighth of the existing scatter budget.
  Larger domains retain the bounded partition or task-local strategy.

## Native streaming output

Every registered streaming accumulator provides a native scalar writer. Sums,
counts, averages, statistics, products, truth and binary reductions now join
extrema on this path. The added writers share a primitive result calculation with boxed output,
including typed nulls, wrapped integers and finite-float canonicalization.
Dense, small-hash, radix and indexed serial emission use the same interface;
parallel emitters publish null metadata after their worker barrier.

Registry tests require this capability for every streaming operation/type pair.
Independent scalar and grouped contracts cover empty/all-null states, arithmetic
overflow, non-finite results, zero weights and covariance. Small-domain task
expansion also observes the heap watermark, including row IDs and final states;
a constrained-budget test checks that it falls back to fewer tasks.

## Mixed payload scatter

Complete all deduplicated input fields for a bounded row chunk before advancing
through each source range. This avoids repeatedly traversing the full output
buffer for binary and mixed streaming reductions. The existing native-width
readers, source selection and partition ownership remain shared. Chunk sizing
bounds the active payload footprint; scratch consists of partition cursors,
without another row-sized buffer.

## Follow-up: core scaling on cache-bounded and legacy-routed shapes

A scaling sweep over 1/8/16/28 threads on a 10M-row grouping input found five
reasons queries stopped speeding up, or slowed down, as cores were added. None
was in the pool; all were routing or footprint decisions.

- **Replicated slab footprint.** The task-local dense strategy gave every worker a
  full slab, and the strategy choice was deliberately independent of cache size.
  Once the slabs together outgrew the last-level cache, every update missed to
  memory: a 100k-group sum measured 6 ms with 8 slabs and 21 ms with 28 slabs on
  the same pool. The runtime now reads the last-level cache size (summed over
  every cache instance, so multi-die parts count each die) and bounds replicated
  slabs to three quarters of it; below three slabs it prefers partition
  ownership, whose per-partition slabs are cache-sized by construction. The
  memory budgets stay data-derived; the cache bound only limits replication.
  Task assignment stays static so floating-point sums remain deterministic for a
  given pool size.
- **Multi-key arithmetic over aggregates.** Any non-aggregate output on a
  two-or-more-key `by:` forced eval-level grouping (no parallelism, ~850 ms),
  because that decision ran before the arithmetic-over-aggregates decomposition.
  Routing now probes decomposability first, and the hidden-slot decomposition
  also admits binary aggregates and literal-probability quantiles.
- **Top-N emit filter.** `desc: AGG take: N` armed an emit filter the parallel
  engine does not implement, so every such query ran on the legacy ladder
  (0.1x parallel at 28 threads for a three-aggregate query). Shapes the
  parallel engine admits with a bounded dense plan now run there and are
  trimmed to the top-N superset; unbounded key domains stay on the ladder
  because the radix route's first-seen ordering and emission tail is still
  serial-heavy there.
- **Filtered prescan.** The selected-row dense plan walked the selection
  serially (~13 ms per 10M rows at any core count); it is now split across the
  pool.
- **Composite symbol keys.** Symbol columns share one domain, so their codes
  interleave and a two-key raw range product overflowed the dense limit,
  sending 10k-group queries to radix (505 ms vs 52 ms on one core). Plans now
  compact keys to the codes they use when the raw product overflows or exceeds
  65,536 slots. The hot loops select the compacted or raw slot computation once
  per run: a per-row check made the two-key loop 12x slower.

Still open: the radix route's post-reduce stages (an input-sized order map to
restore first-seen order, then emission) scale poorly on many-million-group
keys; the legacy ladder remains faster for those top-N shapes. An unordered
`take:` on a grouped select still uses radix's bounded emit rather than the
dense plan.

## Follow-up results: top-N, bounded emit and radix ordering

Five more changes closed the shapes the previous follow-up left open. The
parallel engine now owns the top-N emit filter: radix partitions and dense
finishes finalize the ordering aggregate per group in parallel, keep bounded
candidate heaps, take one threshold from their union and emit only the kept
superset; every other route trims its full result with the same decision,
and the legacy ladder's carve-outs are gone. The radix full path's first-seen
ordering was dispatched by element grain over partitions and chunks, which
produced a single task; it is dispatched by task count and compacts out of
place. An unordered `take: N` on a bounded key selects the N smallest first
rows on the dense task-local path instead of the radix bounded emit. A test
driver flag records which `.rfl` lines still reach the legacy ladder
(`docs/grouping-legacy-census.md`).

Measured on 10M rows (min of five warm runs, ms; before = the branch point,
after = this follow-up):

| Query | 1 core | 8 cores | 28 cores |
|---|---|---|---|
| three-key count, desc take 10 (10M groups) | 962 → 285 | 46 → 54 | 46 → 44 |
| three aggregates by 100k key, desc take 10 | 77 → 41 | 12.8 → 11.0 | 75 → 10.4 |
| where + count by 15-value key, desc take 10 | 451 → 51 | 59 → 9.6 | 23.5 → 6.3 |
| count by 100k key, unordered take 10 | 107 → 17 | 21 → 4.0 | 17.8 → 5.4 |
| count by 100k key, desc take 10 | 28 → 17 | 4.9 → 4.0 | 4.8 → 5.5 |
| sum + count by six keys, no take (10M groups) | 1543 → 1592 | 245 → 201 | 187 → 137 |
| pow(pearson) by two keys | 1840 → 68 | 993 → 12 | 936 → 9.2 |

The remaining legacy routes are enumerated in the census; none of them is a
top-N, bounded-emit or compound-expression shape.
