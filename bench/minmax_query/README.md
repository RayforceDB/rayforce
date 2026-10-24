# Dyadic min/max query routes

A MIN-focused synthetic, value-checked POSIX C probe for SELECT call classification and
avoidable per-row evaluator overhead. It is not ClickBench, a real workload
certification, or a general benchmark framework.

## Build/run (Linux)

Use the **same checkout's headers and static library**. Never combine an older
archive with newer struct definitions. The library build uses the project's
optimized library recipe, including its per-file floating-point flags.

```sh
make -j4 lib CC=clang
clang -O3 -std=c17 -march=native -Iinclude bench/minmax_query/bench.c \
  librayforce.a -lm -lpthread -ldl -o /tmp/ray-minmax-query
RAYFORCE_CORES=0 /tmp/ray-minmax-query 1 200000
RAYFORCE_CORES=3 /tmp/ray-minmax-query 1 200000
```

Arguments are a case number and 1,000–10,000,000 rows, in multiples of 1,000.
`RAYFORCE_CORES` configures background helpers, **not total participants or
proof that those helpers ran**. The C runtime API does not interpret CLI `-c`.

| Case | Timed expression |
|---|---|
| 0 | Direct language `min` over two table columns |
| 1 | SELECT dyadic `min x y` |
| 2 | SELECT `if (< x y) x y`, an equivalent **non-null I64** control |
| 3 | Genuine unary SELECT `min x` reduction |
| 4 | Grouped `sum (min x y)`, with 997 integer keys |

Data creation/runtime initialization happen outside timing. Each fresh process
warms its case twice, then emits five `PRICE sample wall_ms process_cpu_ms`
records. Parsing and query construction are included in each evaluation;
payload checks and result release are outside the timed interval. Process CPU
includes all threads, not just the caller. Every warm/timed result must have
its exact I64 type, length and **every expected value**; success ends with
`VALIDATED 7`. Case 4 computes its oracle independently, not from another
engine route. Run multiple fresh processes in interleaved order and retain
all samples/ranges, including slow controls.

Direct language MIN is still a boxed element-mapping implementation, **not a
raw SIMD MIN kernel**. The IF control is not a null-semantics oracle: language
dyadic MAX treats NULL as low, unlike graph MAX2's null propagation. These
positive-I64 measurements do not certify mixed/narrow types, NULLs, strings,
lexical shadows, file-backed sources, allocation failures or service latency.
Those require separate semantic tests.

An unfixed SELECT that reduces only its first operand will fail the shape/value
checker; its short runtime is not a valid performance baseline. To study the
batching optimization, compare against an arity-correct but unbatched revision.
Record source/binary/compiler/flags, row count, worker settings and affinity.
For helper-free (`RAYFORCE_CORES=0`) controls, optional `taskset` affinity can reduce CPU
placement variance; do not mistake it for exclusive CPU isolation. Do not pin
multiple helpers to one CPU and call that a multicore throughput measurement.
