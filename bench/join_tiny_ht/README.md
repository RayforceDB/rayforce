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
  --cores 1 8 16 --trials 3 --output /tmp/join-tiny-results.json
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
