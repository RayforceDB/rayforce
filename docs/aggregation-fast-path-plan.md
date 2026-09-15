# Query fast-path type coverage plan

This records the original generic coverage requirements. Current scaling work
is tracked in the [grouping engine plan](grouping-engine-scaling-plan.md).

## Objective and scope

Make efficient query execution cover the types and null semantics already
supported by the language. A supported aggregate should not lose the optimized
grouping engine merely because its input is I32, F32, or TIME. A query with
several aggregates should not silently regress when one unsupported
specialization is added.

This census covers all 14 ordinary value kinds (BOOL, U8, I16, I32, I64, F32,
F64, DATE, TIME, TIMESTAMP, GUID, SYM, STR, LIST) across grouped aggregation,
group keys, and the adjacent count-distinct, predicate, top-k, sort, expression,
and output-broadcast paths inspected below. It is not an exhaustive audit of
joins, windows, graph operations, or every arithmetic operator.

Compound/heterogeneous LIST values need an explicit generic strategy, not
reinterpretation as fixed-width numeric data. Preserve deliberately invalid
operations and resource limits. Do not remove a gate until all downstream
readers, null checks, state merges, and output writers support its new cases.

## Evidence

### Runtime census

[aggregation-type-census.csv](aggregation-type-census.csv) records 210 probes:
14 input kinds × 15 unary operations, checking registry presence, scalar result
type, and grouped result-column type. Four rows, two integer-key groups, one
execution core; linked against the baseline release objects. Primitive fixtures
contain 1,2,3,4 (BOOL casts to true); SYM/STR contain b,a,b,c; GUID uses four
generated GUIDs; LIST contains four boxed integers. F32 was constructed with
a query projection cast because the eval-level F32 cast is not supported.
Actual fixture types were checked before the probes.

These probes establish availability and output types, not value correctness,
parallel routing, null behavior, or performance. `ERROR` is an observed error,
not automatically a bug. The operation spellings include `var_pop` and
`stddev_pop`. Binary aggregates and parameterized aggregates were inspected in
source, but are not represented by this CSV.

## Coverage census

### A. Aggregate registry

Source: `src/ops/agg_stream.c:agg_resolve`, with admission in
`src/ops/agg_engine.c:agg_v2_can_handle`.

| ID | Operation family | Registered at baseline | Coverage planned |
|---|---|---|---|
| A1 | sum | I64, F64 | BOOL/U8/I16/I32/F32 and TIME |
| A2 | min/max | I64, F64 | BOOL/U8/I16/I32/F32; DATE/TIME/TIMESTAMP; SYM/STR/GUID |
| A3 | avg | F64 | BOOL/U8/I16/I32/I64/F32 and DATE/TIME/TIMESTAMP |
| A4 | var/var_pop/stddev/stddev_pop | I64, F64 | BOOL/U8/I16/I32/F32 and DATE/TIME/TIMESTAMP |
| A5 | first/last | none | All supported scalar kinds; explicit handling for boxed LIST |
| A6 | prod | none | Numeric inputs, including F32; preserve temporal rejection |
| A7 | all/any | BOOL/U8/I16/I32/I64/F64 | F32; homogeneous boxed input only through a safe generic path |
| A8 | pearson/cov/scov/wsum/wavg | BOOL/U8/I16/I32/I64/F64 inputs | F32, including mixed input pairs; verify each operation's scalar type contract before considering temporals |
| A9 | med, top/bottom K | I64/F64 buffered vtables | Other supported types; buffered admission and memory strategy also required |
| A10 | quantile/percentile, mode | none | Dedicated state/parameter contracts for their existing supported types |
| A11 | count | Type-independent | Preserve count-all-rows behavior; registry presence does not imply key-path admission |

Important distinctions:

- A missing registration is not always the only blocker: `agg_v2_can_handle`
  currently rejects non-streaming vtables, including the registered med/top-K
  vtables. The expression adapter repeats admission restrictions.
- Missing any one required vtable can send the entire grouped query to legacy
  execution. Test mixed queries, not just one aggregate at a time.
- SUM(DATE/TIMESTAMP), PROD(temporals), and ALL/ANY(temporals) remain invalid.
  Do not infer legality from an identical storage width.
- On these runtime probes, F32 reductions returned F64, including min/max and
  first/last. Preserve current behavior until a separate semantic decision is
  made; do not assume every extreme must return its input type.
- Narrow integer and temporal extremes retain their logical types; TIME sum
  returns TIME. Integer sums/products return I64, and means/statistics return F64.
- Scalar med(BOOL/F32) succeeds but grouped med(BOOL/F32) errors on the fixture.
  Treat these as correctness investigations in the first work package, before
  attempting to use grouped legacy execution as an oracle for those cells.
- The LIST fixture contains only numbers. It establishes no contract for
  arbitrary mixed objects, nesting, or null cells.

### B. Keys, nullability, and strategy selection

| ID | Gap | Evidence and required work |
|---|---|---|
| K1 | Nullable integer/temporal keys lose v2 dense and radix | `agg_dense_plan`, `agg_dense_plan_sel`, and `exec_group_v2_run` reject may-have-null metadata for non-SYM keys. Add explicit null-key representation in dense/radix plans. |
| K2 | Float, GUID, STR, LIST keys cannot enter v2 | `agg_v2_can_handle` admits integer/temporal/SYM only. Some lower helpers/comments mention STR/F64 fallback, but they do not constitute an end-to-end supported route. Add typed hash/equality/gather support before admission. |
| K3 | Generic parallel hash overallocates | `exec_group_v2_parallel` allocates each worker for input-sized group/state capacity and a hash table sized from total rows. Bound memory as key support expands; avoid O(workers × input) slabs. |
| K4 | Legacy alternatives have narrower aggregate support | Legacy direct-insert accepts COUNT/SUM/AVG only, and nullable inputs are declined; legacy dense/sparse key gates also reject nullable non-SYM. Prefer a complete v2 route over duplicating every new kernel in legacy code. |

K1 is necessary for nullable temporal grouping: merely registering MIN(TIME) can move the
query to a different expensive fallback. Nullable signed integer sentinels may
be used in a proven canonical representation; null must remain distinct from
zero and ordinary extreme values. Dense planning must exclude null from min/max
range estimation and budget a separate null slot. Composite keys need a null
identity per component. BOOL/U8 vectors are non-nullable in this revision;
do not invent a null bitmap contract for them.

For floats, grouping must canonicalize signed zero and null/NaN consistently
with existing equality and distinct semantics. For symbols, preserve adaptive
width and dictionary domains. STR/GUID need full values or stable row references,
never truncation to an int64. Generic LIST keys retain deep equality semantics.

### C. Related query fast paths

| ID | Confirmed coverage restriction | Source |
|---|---|---|
| Q1 | Fused count-distinct accepts I16/I32/I64/SYM only; omits BOOL/U8/temporals and floats/wide types | `cdfuse.c:cdf_type_ok`; caller's two-stage rewrite admits more integer/temporal types |
| Q2 | SYM fused count-distinct is unreachable through its null gate | `ray_cd_fused` rejects `ray_vec_may_have_nulls`, which always returns true for SYM; the outer rewrite uses the exact null test instead |
| Q3 | Fused predicates omit F32/F64, GUID, ordinary non-dictionary STR comparisons; nullable numeric/temporal columns are declined | `fused_pred.c:fp_atom_col_compatible`, predicate admission and `fp_col_supported_op` |
| Q4 | Fused filter + top-k sort keys omit F32/F64 and GUID | `fused_topk.c:ray_fused_topk_select` type gate; predicate support is a separate gate |
| Q5 | Composite radix sort excludes F32 when there is more than one sort key | `sort.c:sort_indices_ex`, `t == RAY_F32 && n_cols != 1` |
| Q6 | Typed literal broadcasts omit F32/GUID/STR | `query.c:can_atom_broadcast`, `atom_broadcast_vec`; fallback allocates per-group cells |
| Q7 | Query numeric admission omits F32 while lower expression code admits it for pow | `query.c:dag_numeric_type_admitted` / `dag_pow_type_admitted` vs `expr.c:ew_pow_type_admitted` |
| Q8 | Temporal arithmetic inside aggregate expressions forces eval | `query.c:expr_contains_temporal_arith`, `is_group_dag_agg_expr_dag_safe`; a semantic guard for units and result types, not a safe gate-only change |

Wide STR/GUID sort already has rank/comparison alternatives; calling that
fallback does not establish that it is slow. Benchmark before replacing it.
Similarly, Q3 gates protect null comparisons and temporal conversions. Reuse
the existing comparison contract (including null ordering), not SQL-style
assumptions about null predicates.

Q1/Q2 changes must preserve `(count (distinct x))`, including how null contributes
to distinct, rather than adopting SQL COUNT(DISTINCT) semantics. An exact
null-free scan can restore SYM admission initially, but measure its cost; a
null-capable canonical code kernel is the eventual route.

## Implementation sequence

Each work package should be reviewable and leave fallback execution correct.
The order below is the intended delivery order; dependencies are explicit.

### P0 — Lock semantic contracts and expose routing

Covers all IDs; prerequisite for new admission.

1. Turn the census into maintained, table-driven contract cases in
   `test/test_agg_registry.c`, `test/test_agg_engine.c`, and Rayfall query tests.
   Separate language legality, result type, registry support, and strategy
   support. Include scalar and grouped values, not only availability.
2. Resolve the BOOL/F32 grouped-med discrepancies with minimal reproducers and
   explicit expected semantics. Check scalar/grouped F32 return-type behavior
   across filtered, unfiltered, serial, parallel, and persisted inputs.
3. Add test-visible strategy counters or a diagnostic record: admitted engine,
   chosen strategy, and decline reason (unsupported input/output type,
   nullable key, buffered aggregate, expression, resource budget). Integrate
   optional profiler reporting without adding work inside hot row loops.
4. Expand fixtures to every legal op/type cell, binary mixed-type pairs, and
   parameterized aggregates. Track intentional rejection cells explicitly.

Acceptance: tests can prove that the intended fast path ran; an equal answer
from fallback alone cannot satisfy a fast-path regression test.

### P1 — Complete typed accumulator plumbing

Covers prerequisites for A1–A10 and K2. Depends on P0.

- Extend `agg_acc.h` validity to F32; its current switch handles F64 NaN and
  signed sentinels, but defaults other types to valid. Resolve slice validity
  once per batch. Keep a no-null loop selected outside the row loop.
- Introduce typed input readers/kernels; do not alias TIME/I32/F32 to an I64
  pointer. Centralize logical result type and accumulator storage type as
  distinct concepts.
- Replace both `agg_put_cell` and `agg_put_cell_value`: today every non-F64
  scalar output is written as int64. That would overwrite narrow output
  buffers if new narrow/temporal registrations were simply enabled.
- Keep parallel payload writes disjoint and merge null metadata after workers
  finish. Audit serial, dense, generic hash, radix, and partition emitters.
- Prepare a source-column/row context for value-preserving extrema and ordered
  aggregates. The existing vtable has no source-row parameter in update_batch;
  parallel first/last needs original row identity, including filtered input.
- Support native-width bulk finalization where useful to avoid allocating one
  temporary scalar per output group. Keep ownership and OOM cleanup explicit.

Acceptance: width-safe output under ASan/UBSan; typed nulls and domain/owner
lifetimes correct; no new registration enabled before its full pipeline works.

### P2 — Numeric and temporal streaming coverage

Covers A1–A4, A6–A8. Depends on P1.

- Generate or share typed sum/min/max/avg/prod/statistics loops for every legal
  numeric/temporal cell in section A. Add F32 boolean and binary statistics
  readers and mixed-pair dispatch; retain temporal legality checks per op.
- Preserve unsigned-wrap integer reduction behavior, float normalization,
  zero/empty identities, all-null results, and sample/population distinctions.
  Do not combine this change with a new statistical algorithm or precision policy.
- Admit the same capabilities in plain-column and materialized-expression
  adapters. Route mixed aggregate sets together where the engine supports all
  states. Ensure COUNT still counts input rows, including null values.

Acceptance: full op/type value parity, exact result types, boundary integers
above 2^53 for integer extrema, and no regression for existing I64/F64 kernels.

### P3 — Nullable keys and bounded grouping memory

Covers K1/K3/K4. Depends on P1; combine with P2 for nullable-key acceptance.

- Add null-aware dense slot mapping and radix key encoding, including selected
  rows, all-null columns, composite keys, and null-preserving key emission.
- Use byte-aware, overflow-safe memory budgets for worker slabs and scatter
  buffers. Grow generic hash tables with observed groups or partition input
  so each worker does not reserve total-input capacity.
- Retain sparse-range and excessive-domain fallback; never allocate a dense
  array spanning a signed null sentinel to the largest real key.
- Benchmark dense vs radix using actual range/cardinality and worker count.
  Do not optimize a particular workload by an unconditional dispatch override.
- Leave legacy as a correctness fallback. Only extend its direct-insert merge
  operations if profiling shows a remaining workload that v2 cannot address.

Acceptance: Nullable-key MIN(TIME) uses an efficient null-aware route and improves
repeatably against the same-machine baseline, with identical typed results and
reported scratch/RSS. No hardcoded claimed speedup before measurement.

### P4 — Ordered and wide-value aggregates

Covers A2/A5 and the scalar-value part of A9/A10. Depends on P1/P2.

- Add first/last using source-row position, not worker merge order. Match the
  established skip-null semantics. Preserve original row positions through
  selection compaction, radix scatter, expression materialization, and partitions.
- Add SYM/STR/GUID min/max with existing comparators. SYM order is lexical by
  domain strings, not intern-code order. Use winning row references for wide
  values and gather after merging; retain source owners until emit completes.
- Handle LIST first/last and comparable extremes through explicit value/row
  semantics. Do not specialize heterogeneous arithmetic by guessing from one row.

Acceptance: correct results across shuffled worker completion, cross-partition
groups, multiple SYM domains/widths, pooled strings, GUIDs, and null-only groups.

### P5 — Float and wide grouping keys

Covers K2. Depends on P3/P4's key/value utilities.

- Add F32/F64 canonical hash/equality, then GUID and STR key strategies; use
  dictionary codes for STR only with a valid domain and matching equality.
- Replace integer-only tuple readers in every newly admitted path. Direct
  int64 interpretation of F64 or truncation of GUID/STR is not an implementation.
- Allow LIST keys through a bounded generic grouping strategy that can share
  typed aggregation where legal. Preserve deep comparison and ownership.
- Handle composite keys and selections at the same time as single keys; keep
  existing supported key-count limits explicit and consistent.

Acceptance: grouping/distinct parity for ±0, canonical/noncanonical NaNs,
nulls, duplicate wide values, and composite keys, with bounded memory.

### P6 — Buffered and parameterized aggregates

Covers A9/A10. Depends on P0/P1/P3/P4.

- Add missing typed med/top-K/bottom-K kernels and quantile/mode capabilities
  only for existing legal input types. Define parameter forwarding once,
  including quantile probability, K, ties, empty groups, and null handling.
- Replace the blanket streaming-only admission with strategy-specific support:
  buffered state requires a proven memory plan. The existing serial driver and
  streaming guards cannot simply be bypassed for large data.
- Share group IDs/row slices for mixed streaming and holistic aggregates where
  practical; avoid regrouping the same input once per unsupported aggregate.
- Give buffered state complete merge/destruction/OOM/cancellation behavior.
  Consider bounded top-K storage rather than retaining every row by default.

Acceptance: all supported input types have an explicit optimized or bounded
generic route, and large-group med/top-K cannot multiply memory by worker count.

### P7 — Count-distinct coverage

Covers Q1/Q2. Depends on P0/P3; float/wide extensions depend on P5.

- Restore reachable SYM specialization with correct null handling.
- Add BOOL/U8/DATE/TIME/TIMESTAMP, then float and wide representations using
  the shared key contracts. Align the rewrite and kernel admission predicates.
- Preserve the 16-byte composite packing limit of the current rewrite until
  a different representation is implemented; do not just widen the type gate.
- Cover nullable group keys and nullable distinct values independently.

Acceptance: dispatch tests for SYM and temporals, value parity including null
distinctness, and end-to-end timings including any admission prescan.

### P8 — Predicates, top-k, and sort

Covers Q3–Q5. Depends on P0 and shared comparison/key contracts from P5.

- Add F32/F64 fused comparison operands and constants with correct mixed numeric
  comparison, NaN/null semantics, and precision for large integer constants.
- Add nullable numeric/temporal predicate evaluation; preserve exact temporal
  type/unit compatibility unless explicit conversion is compiled.
- Add GUID equality/order and ordinary STR comparisons where reusable kernels
  make them worthwhile. Preserve existing SYM/STR equality semantics.
- Add float/GUID top-k sort keys with existing ordering/null/tie contracts;
  share comparator semantics with the general sorter.
- Enable composite F32 radix sorting using a proven float sort-key transform.
  Benchmark existing rank fallback before attempting a new wide-key radix path.

Acceptance: fused/unfused differential values and row ordering under ascending,
descending, ties, nulls, ±0 and NaNs; large-input dispatch assertions.

### P9 — Expression admission and output construction

Covers Q6–Q8. Depends on P0/P1; temporal aggregate integration depends on P2.

- Add F32/GUID/STR broadcasts, with proper STR pool/GUID representation and
  null metadata. For LIST output, retain explicit per-cell ownership semantics.
- Reconcile F32 query numeric admission with the lower expression executor
  operation by operation. Preserve promotion and rounding, not just pow output.
- Compile legal temporal arithmetic with explicit unit conversion and logical
  result type before allowing it inside fused aggregate expressions. Keep
  semantically invalid temporal combinations rejected.
- Replace duplicated capability lists where practical with operation-specific
  helpers shared by admission and execution. Do not introduce one overly broad
  `is_numeric` predicate for operations with different contracts.

Acceptance: adding an F32 expression or typed literal to a grouped query does
not unexpectedly trigger per-group evaluation or boxed output construction.

### P10 — Regression and performance closure

Depends on all work packages. Each preceding package also runs its own checks.

- Retire a legacy restriction only after its relevant cases have parity and
  dispatch coverage. Keep documented generic fallbacks for heterogeneous data
  and genuinely unsupported strategy shapes.
- Run the complete C/Rayfall suite (`make test`), relevant sanitizer checks,
  and targeted race checks for new parallel writers/state merges. Build the
  release binary for timings; never benchmark the sanitizer binary.
- Update the census with result semantics and reachable optimized strategies,
  and record accepted intentional fallbacks rather than leaving silent holes.

## Validation matrix and performance protocol

Every newly admitted op/type cell needs both correctness and path evidence.
Use table-driven semantic tests and selected cross-products, not a huge set of
tests that merely repeats each switch case.

| Dimension | Required cases |
|---|---|
| Values | empty, singleton, all-null where representable, mixed nulls, zero, extremes, overflow, NaN normalization, values above 2^53 |
| Layout | owned vectors, nonzero-offset slices, gathered/filtered inputs, mapped/splayed columns, empty and nonempty partitions |
| Groups | one, low cardinality/dense range, sparse range, high cardinality, all unique, skewed, composite, null keys vs zero keys |
| Parallelism | one core, a fixed multi-core count, automatic pool; groups crossing morsels and partitions |
| Query shape | scalar, grouped, multiple mixed aggregates, WHERE selections, aggregate expressions, sort/take and literals |
| Metadata | BOOL/U8 non-nullability, SYM W8/W16/W32/W64 and domains, STR owners/pools, typed null propagation |
| Failures | bounded allocation failure, cancellation, partial accumulator initialization, fallback cleanup |

Compare optimized results to established scalar operations per group where
their semantics agree, plus hand-calculated edge cases. Retain order-aware
oracles for first/last and sort/take. Use tolerances only for floating reductions
where reduction order permits them; integer/temporal extrema must be exact.

Performance fixtures:

1. Deterministic synthetic fixtures with nullable I32 keys and TIME values,
   varying row counts, cardinalities and skew.
2. Same workload with I16/I32/I64/F32/F64/TIME/TIMESTAMP value columns and legal
   aggregates, nullable and null-free variants. Measure mixed aggregates too.
3. Low/high-cardinality SYM count-distinct, temporal count-distinct, float
   filter+top-k, composite F32 sort, and high-group-count literal broadcasts.
4. Existing representative I64/F64 workloads and tiny queries to catch added
   planning/prescan overhead; wide-key cases to validate fallback economics.

Run before/after binaries sequentially in alternating order with identical
core count, data, build flags and CPU conditions. Report warm median and spread,
cold allocation separately, phase times, scratch high-water mark/RSS and chosen
strategy. Include input conversion and admission scans in end-to-end query cost.
Agree per-workload regression tolerances from measured baseline variance; do
not use a universal nanosecond threshold in unit tests.

## Completion criteria

- Every census gap A1–A11, K1–K4 and Q1–Q8 is mapped to an implemented route or
  an explicit, justified generic/rejected case with tests.
- Nullable-key temporal grouping gains a measured improvement from an efficient nullable-key temporal
  aggregation route, with exact result/null/type parity.
- No new admission can reach an incompatible reader, emitter, validity check,
  or unordered merge implementation.
- The suite checks fast-path reachability, so future type additions cannot
  silently pass correctness tests by dropping back to a slower engine.
