# Grouping engine scaling plan

Status: implementation and acceptance in progress. Dense, shared indexed, wide
consumer and distinct changes are implemented locally; the final worker sweep
and delivery are pending. This extends the type-coverage plan.

Acceptance compares immutable release binaries with identical generated inputs.
The rebased ASan/UBSan suite passes 3,824/3,824 tests. Current measurements
and reproducible commands are in [the results report](grouping-engine-scaling-results.md).

## Objective

Remove avoidable grouping, allocation, synchronization, and merge overhead for
all supported grouped aggregate families. Preserve types, nulls, ordering,
overflow behavior, symbol domains, and cancellation. This work covers grouping
and its query integration, not unrelated joins, CSV parsing, or sorting queries.

Equal latency across aggregates is not an acceptance criterion. Sum consumes
every valid value; extrema can avoid updates; exact median and distinct need
additional data structures. Each family needs its own baseline and scaling proof.

## Current implementation and gaps

| Family / shape | Current execution | Remaining proof or implementation |
|---|---|---|
| BOOL/U8/I16/I32/DATE/TIME min/max, eligible dense single key | Shared state through explicit concurrent-update capability | Adversarial value order, key skew, nullable values, contention crossover |
| Numeric sums, count, average, statistics, product, boolean and binary reductions | Partitioned dense for eligible single keys; task-local or radix otherwise | Separate aggregate scaling on synthetic fixtures, mixed aggregates, payload and state traffic |
| I64/TIMESTAMP/F32/F64 extrema | Partitioned dense | Measure before considering additional shared kernels; preserve NaN and null contracts |
| Selected and composite dense keys | Partitioned native payload; selected source-row indices shared across inputs | Final repeated scaling sweep |
| Sparse integer/temporal/SYM keys | Radix | Measure scatter, skew, merge and output costs across cardinalities |
| Float, STR, GUID and other supported wide keys | Shared parallel directory with complete key equality and deterministic first-row IDs | Final repeated scaling sweep and race checks |
| First/last, wide extrema, median, quantile, mode, top/bottom K | Shared stable row slices; row-balanced groups and splitting within dominant groups | Final repeated scaling sweep and race checks |
| Count-distinct | Pair-hash partitioning; parallel radix ordering and output | Large ordering oracle and final repeated scaling sweep |
| Mixed streaming/indexed aggregates and expressions | Shared groups and stable row slices; streaming tasks split by rows | Final repeated scaling sweep |

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
worker sweep for every family above. The final synthetic sweep remains outstanding.

### G1 — Close the dense streaming refactor

1. Measure histogram, scatter, reduction, partial merge and output separately
   for sum/count/statistics, not just shared extrema.
2. Reduce duplicated input fields and state traffic where profiles justify it.
3. Tune partition tasks using work and state size, with bounded split storage;
   validate a dominant null key and a dominant non-null key.
4. Keep strategy choice capability-driven. Add shared updates only where safe
   and measured; contended atomic sums are not an automatic replacement.
5. Check default uses all logical CPUs; distinguish pool size from bounded tasks.

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

## Implemented changes awaiting final acceptance

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

Full ASan/UBSan validation passes 3,824/3,824 tests; targeted TSan passes 6/6.
Performance acceptance and delivery remain open.

## Dominant-group execution stages

Assigning a whole group to one worker leaves a single large group serial even
when grouping itself scales. Consumer scheduling therefore considers rows
within groups as well as independent groups:

- First/last and wide extrema select candidates from contiguous row chunks and
  merge them in original index order. Null-only chunks contribute no candidate.
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
