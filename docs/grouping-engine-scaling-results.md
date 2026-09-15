# Grouping engine scaling results

Status: synthetic performance acceptance is pending. This report contains only
synthetic fixtures and generic engine validation.

## Method

Use separate immutable release binaries, identical generated inputs and worker
settings 1, 2, 4, 8, 16 and default. Run binaries sequentially, reversing their
order between rounds. Record the first query separately from five warm queries
in each of three fresh processes. Retain timing ranges and peak RSS.

The default pool uses all 28 logical CPUs on the validation host. Typed result
comparisons check keys, nulls and integer values exactly; floating reductions
use relative tolerance `1e-9` and absolute tolerance `1e-8`. Independent unit
oracles cover ordering, nulls, rank and distinct semantics.

## Correctness

- Complete ASan/UBSan suite: **3,824/3,824 passed**.
- Targeted TSan: **6/6 passed**, three/four workers, `setarch x86_64 -R`, no suppressions.
- Coverage includes native widths, source symbol domains, structural LIST keys,
  stable row indices, dominant-group exact `med`/quantile, frequency ties,
  parallel top/bottom-K merges, adaptive symbol slices and typed empty output.

Four synthetic cases fail in the baseline: STR-valued count-distinct and its
mixed form crash; LIST-key count-distinct and its mixed form raise a type error.
These cases receive no baseline speedup claim. Candidate results are checked
across workers and with independent unit-test oracles.

## Reproduction

```sh
python3 bench/groupby_shapes/grouping_scaling.py \
  --binary /path/to/candidate --baseline /path/to/baseline \
  --baseline-skip-cases distinct-string-values,mixed-distinct-string-values,distinct-list-keys,mixed-distinct-list-keys \
  --workers 1,2,4,8,16,default --rounds 3 --output /tmp/grouping.json

python3 bench/groupby_shapes/summarize_scaling.py /tmp/grouping.json /tmp/grouping.csv
```
