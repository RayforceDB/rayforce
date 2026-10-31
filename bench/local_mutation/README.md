# Local mutation (#950)

The workload appends 500 rows to an ordinary three-column table and amends
10 elements of a 500-element vector per step. Each run starts from empty
state. The script compares named global mutation with named local mutation,
prints row counts and final vector sums, and reports median elapsed times
from five samples after a short warmup.

```sh
make release -j4
./rayforce -c 1 -f bench/local_mutation/local_mutation.rfl
```

To measure the previous functional-rebinding workaround separately:

```sh
./rayforce -c 1 -f bench/local_mutation/functional.rfl
```

Use the same optimized build and worker count for both workloads. Compare
1,000 with 2,000 steps to distinguish linear append scaling from repeated
whole-table copying. Avoid timing assertions in the test suite: results
depend on the host, allocator state, and other running work.

This change reuses the existing table-append and indexed-vector-amend fast
paths. Other operations and representations retain their existing allocation
behavior. Physical pointer checks in `lang/local_mutation_fast_paths` verify
that unique ordinary local table/vector fast paths engage, while a real
alias still forces a copy.
