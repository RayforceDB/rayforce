# Typed map result collection

The workload compares homogeneous integer results, explicit conversion after
mapping, early and late numeric type changes, nested results and repeated
string atoms. It checks each run's result and prints the elapsed time and
process peak tracked heap bytes. No timing assertion is part of the tests.

## Reproduce

Build the baseline and candidate with the same compiler and flags. For the
original baseline, use `dev` at `4529eb3c`:

```sh
git worktree add --detach /tmp/rayforce-map-before 4529eb3c
make -C /tmp/rayforce-map-before release -j4
make release -j4
python3 bench/map_typed/compare.py \
  --before /tmp/rayforce-map-before/rayforce --after ./rayforce \
  --samples 5 --n 100000 --reps 5 --output /tmp/map-results.json
```

The harness runs each build in a fresh process with one core and alternates
which build runs first. `ms` is the median elapsed time for all repetitions;
peak bytes is the largest process peak reported by `.sys.mem`, not RSS.
Raw measurements are optional local output, rather than committed host data.
The harness needs only Python's standard library.

To run one case directly:

```sh
MODE=typed N=100000 REPS=5 ./rayforce -c 1 -f bench/map_typed/map_typed.rfl
```

`typed` maps increment and sums the result. `postcast` explicitly casts that
map result to I64 before summing: on the baseline this measures list
collection followed by conversion, a reference for a post-collection
approach. `mixed` alternates I64/F64 atoms; `late` changes only the final
atom to F64. `nested` returns singleton vectors and counts the outer list.
`strings` returns one repeated string atom and counts the result. Timings
include callback execution, the loop and final sum/count, so they are not
isolated collector timings. In particular, typed output also changes which
sum path is used.

## Local observations

Measured on 2026-10-23 for the initial typed collector (`05c8d953`),
with both builds using the repository's Clang release flags
(`-O3 -march=native`), and the defaults above. These are illustrative
measurements from one machine, not portable performance guarantees.

| Mode | Before ms | After ms | Before peak bytes | After peak bytes |
|---|---:|---:|---:|---:|
| typed | 34.681 | 29.622 | 8584384 | 2283904 |
| postcast | 43.248 | 33.008 | 9599808 | 3199808 |
| mixed | 58.507 | 58.305 | 8584896 | 8584896 |
| late | 55.480 | 55.444 | 8584896 | 9600576 |
| nested | 34.000 | 32.949 | 8584384 | 8584384 |
| strings | 16.012 | 19.202 | 2184384 | 3232960 |

Direct homogeneous integer collection uses about one quarter of the old
peak tracked heap and avoids the explicit cast. Early mixed and nested
results remain lists. A late mismatch temporarily holds the typed prefix
and its reboxed list, increasing peak memory. The repeated-string case is
slower and larger: the old list shares one string atom, while the typed
string vector stores a descriptor for every row. Typed results are a
language behavior change, not a promise that every representation is cheaper.

`test/rfl/mem/map_typed_peak.rfl` separately checks a generous peak-memory
budget for homogeneous integer output, without wall-clock assertions.
