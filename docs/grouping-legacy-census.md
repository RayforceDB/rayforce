# Grouping: census of shapes still served by the legacy ladder

Status: recorded 2026-09-19 after the core-scaling follow-up. The parallel
grouping engine ("v2") now owns every top-N emit filter, unordered bounded
emit, multi-key arithmetic over aggregates, and composite symbol keys. This
census lists what still reaches the legacy grouping ladder in
`exec_group_run`, as the factual starting point for retiring it.

## How it was measured

The test driver records, for every evaluated `.rfl` line, whether the
grouping executor took the legacy route and the admission reason it was
given:

```
make test                     # builds ./rayforce.test
RAYFORCE_CORES=2 ./rayforce.test --census census.tsv -f rfl/
cut -f1 census.tsv | sort | uniq -c
```

Each census line is `reason<TAB>file:line<TAB>source`. Run it with the
suite's worker count: one admission reason (`parallel_wide`) depends on the
pool size, and several ordering assertions in the corpus assume two workers.

## Results (514 `.rfl` files, 340 legacy hits)

| Reason | Lines | With `by:` | What it means | What v2 needs |
|---|---:|---:|---|---|
| `shape` | 200 | 5 | 195 are scalar aggregations with no keys (`select {s: (sum v) from: T}`), mostly over partitioned stores; the 5 grouped ones are nested selects whose outer scalar select carries the reason, plus one `by:` on a missing column | A keyless (single-group) path in v2, or routing scalar aggregation to the vector aggregators; the grouped 5 are already served |
| `agg_expression` | 63 | 63 | Aggregate over an expression: `(sum (strlen s))` ×35, `(sum (at ...))` ×15, `(first (at ...))` ×14, `(avg (strlen s))` ×11, `(count (select ...))` ×7 | The expression-input path (`exec_group_v2_exprs`) materializes inputs it can evaluate as a full column; string-length and indexed/nested inputs are declined today |
| `key_expression` | 31 | 27 | Computed keys: `(+ k 1)` ×10, `(xbar ts N)` ×5, `(substr ref 0 3)` ×2, `(minute EventTime)`, dotted temporal accessors `ts.date` | Materialize computed keys as scan columns before admission, the way expression aggregate inputs are; temporal accessors need the same |
| `parallel_wide` | 30 | 30 | Float keys with 2 to 8 workers and no indexed aggregate (`by: {f: f}` on F64) — a measured choice to prefer the ladder's directory over v2's replicated float hash | Make the v2 float route competitive at small pools (or drop the pool-size gate once measured) |
| `admitted` | 16 | 15 | Admitted by v2 but executed on the ladder: partitioned-store sources (`from: Pmc by: date`) where the per-partition executor merges partial groups on the legacy path, and one derived-key symbol-domain case | A partition-aware merge in v2 (streaming states merge; buffered ones need the ladder's row slices) |

No line reached the ladder because of an emit filter, a bounded emit, or a
multi-key compound expression; those classes are closed.

## Reading the table

- Two classes are not grouping at all: keyless scalar aggregation (195 lines)
  and the outer scalar select of a nested query. They keep the ladder alive
  for reasons unrelated to group-by scaling and can be moved independently.
- The grouped remainder is 140 lines in four classes. Three of them
  (`agg_expression`, `key_expression`, `admitted` on partitioned sources) are
  admission gaps that a materialize-then-scan step closes without a new
  strategy; `parallel_wide` is a performance gate that needs a measurement,
  not a feature.
- The benchmark query files add two shapes the corpus lacks: a computed key
  through several nested string functions, and a `where:` filter on the
  grouped result (`(> c 100000)`), which the emit filter already expresses.

## Next step

Retiring the ladder is its own plan: close the four grouped classes above,
route keyless aggregation to the vector aggregators, then delete
`exec_group_run` and the consumers it alone uses.
