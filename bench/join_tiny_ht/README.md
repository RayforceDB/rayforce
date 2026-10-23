# Tiny-build chained join probe

A fixed-key version of the issue #599 shape: 200 inner joins of a 130,000-row,
773-instrument table against three unique composite keys. Each run verifies
101,400 output rows by an independent arithmetic oracle. This is a count check,
not a full answer-equivalence proof; correctness fixtures check exact pair IDs.

Build baseline and candidate **release** binaries in separate worktrees and
preserve copies. `make test` overwrites the worktree CLI with a sanitizer build;
do not accidentally compare that binary against release.

```sh
python3 bench/join_tiny_ht/compare.py /path/base-release /path/candidate-release \
  --cores 1 4 --trials 5 --output /tmp/join-tiny-results.json
```

The Unix-only, standard-library Python wrapper shuffles fresh processes, records
binary and probe SHA256s, and explicitly selects the existing steal scheduler.
`-c N` is the CLI's **total participants** (main included); this is not the
background-worker count used by positive `RAYFORCE_CORES`. The script clears that
environment override and tracing. There is no explicit warmup. Process user/sys
CPU and process wall time include startup/setup; `loop_ms` measures the join loop.
Avoid concurrent local builds/tests and report hardware, compiler, affinity and
noise/ranges when sharing measurements. The wrapper does not certify build flags
or exclusive machine idleness.

This is a narrow workload, not a general join or compression benchmark, a pool
policy recommendation, or complete resolution of issue #599. User CPU is not an
instruction count: clocks, SMT, topology and waiting can change thread-seconds.
For causal analysis, measure instructions/cycles separately with helper-thread
inheritance and multiplexing checks. Do not mix instrumented timing with the
uninstrumented comparison.

## What this optimization changes

The minimum chained table has 256 heads (1 KiB). Its count and fill passes do
not hash a future left row merely to prefetch a head in that small table. Actual
current-row hashing, equality and output are unchanged. Capacity, not distinct
key count, selects the mode once per pass; the 128/129 right-row boundary has
256/512 heads. Both serial and pooled calls use the selected callback. Shared
inline bodies keep the semantics in one place while the two constant-mode
wrappers avoid a release per-row capacity-mode test in the inspected Clang build.
The larger-table prefetch and build-side prefetch remain. The head array's size
is not proof that the entire working set stays resident.

Exact pair-ID fixtures separately cover duplicates, nulls and misses for inner,
left and full joins, the capacity boundary, and probe rows spanning a morsel.
The benchmark's row-count oracle does not replace those checks. Choose core
counts appropriate for the machine; requested counts do not prove which physical
helpers executed tasks. The example uses one and four total participants rather
than silently oversubscribing a four-core host. Retain any unfavorable larger,
radix or long-string controls when reporting the narrow gain. Callback
specialization can change code size/layout even on a retained-prefetch path.
