# Aggregation and query type coverage

This records earlier type-coverage work. Current implementation and validation
are tracked in the [grouping engine plan](grouping-engine-scaling-plan.md) and
[results report](grouping-engine-scaling-results.md).

## Implemented coverage

| Plan | Gaps | Final implementation |
|---|---|---|
| P1 | Plumbing for A1–A10 | Native input widths, F32 NaN validity, type-correct scalar writes, disjoint parallel payload writes with null metadata merged afterward. Native extrema emission avoids temporary atoms. Stable source row indices retain column/domain ownership for ordered and wide values. |
| P2 | A1–A4, A6–A8, A11 | Streaming sum/min/max/avg/product/statistics across legal numeric/temporal types. F32 truth and mixed binary statistics. COUNT counts every row, including nulls. DATE/TIMESTAMP sum and temporal product/truth remain illegal. |
| P3 | K1, K3, K4 | Nullable dense mapping reserves a null slot without spanning the sentinel; selected/composite/all-null keys use the same mapping. Signed radix keys preserve nulls. Dense worker/global slabs have allocation and sampled traffic budgets; setup, merge and scalar emission are parallel. Removed full-input-per-worker generic hash allocation. |
| P4 | A2, A5 | First/last use stable original-row order, including selections; wide extrema use source rows and existing comparators. SYM extrema compare domain strings. LIST first/last retain selected values. |
| P5 | K2 | Canonical float hashing/equality, GUID bytes, STR bytes and structural LIST grouping. Small/ordered/buffered shapes use one shared index layout. Plain LIST-key aggregate queries, including composite keys, reach it through `select`. Large float/GUID/STR streaming shapes with up to eight keys use the existing parallel wide-key engine. |
| P6 | A9, A10 | Shared group IDs and stable row slices for median, quantile, mode, top/bottom-K, ordered/wide and mixed streaming aggregates. Wide/F32 top-K stores at most K row indices. Existing typed numeric heaps and rank helpers remain shared. |
| P7 | Q1, Q2 | Fused exact count-distinct accepts nullable BOOL/U8/signed/temporal/SYM/F32/F64 pairs. SYM is reachable despite conservative null metadata. F32 generic exact counting widens losslessly once to reuse F64 dedupe. |
| P8 | Q3–Q5 | F32/F64, nullable numeric/temporal, GUID and ordinary STR fused comparisons. Float/GUID filter/top-k sorting. Composite F32 sorts reuse the tested single-column float transform and radix-compose ranks. |
| P9 | Q6–Q8 | Typed F32/GUID/STR broadcasts, F32 numeric/pow admission, and one-time materialization of pure temporal arithmetic through unit-aware operations before typed aggregation. |

The [census](aggregation-type-census.csv) preserves the original 210 observations
and adds current streaming registration and grouped route columns. Registry
presence and query strategy are different: holistic/ordered operations use row
indices instead of streaming accumulator states.

## Explicit generic and rejected cases

- Heterogeneous LIST arithmetic/extrema/rank operations retain dynamic value
  evaluation. No inference from the first element selects a fixed-width kernel.
  Structural LIST keys and LIST first/last have a bounded shared-index route.
- GUID/STR/LIST count-distinct uses exact generic dedupe. The packed count-distinct
  rewrite still requires total key/value width at most 16 bytes; wide values
  are not truncated or interpreted as integers.
- Large float/wide streaming keys retain the established parallel implementation.
  On one million rows, moving these to serial index grouping regressed STR and
  F32 key workloads by about 3x. Admission now reports `parallel wide-key strategy`.
  Shapes needing ordered/buffered states, LIST keys, and more than eight keys
  use the bounded index route.
- Nullable LIKE/IN, SYM ordering predicates, and cross-unit temporal comparisons
  retain their existing evaluators. The new fused comparison leg covers the six
  scalar comparison operators; it does not silently extend other evaluators.
- Raw temporal DAG arithmetic stays guarded. Pure `+`, `-`, and `*` expressions
  are materialized once using existing unit/promotion rules. Other expressions,
  side-effecting calls, and unsupported partition shapes retain existing evaluation.
- Eval-level `as 'F32` remains outside this change's language surface. F32 test
  and benchmark fixtures use the existing query projection cast or C constructors.
