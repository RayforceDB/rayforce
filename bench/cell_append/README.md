# Reserved-capacity append cost

This POSIX C-API microbenchmark isolates an append that does **not** resize.
It allocates and touches the destination before timing, then validates every
value (or LIST slot) after timing. It is not a whole-engine or ingestion benchmark,
and does not measure the memory benefit or cost of natural capacity growth.

Linux build/run example:

```sh
make -j4 lib
clang -std=c17 -O3 -march=native -DNDEBUG -Iinclude \
  bench/cell_append/append.c librayforce.a -rdynamic -lpthread -lm -ldl \
  -o /tmp/cell-append
RAYFORCE_CORES=0 taskset -c 0 /tmp/cell-append vec 2000000
```

Cases: `vec` (single I64 append), `raw` (raw append with count1), `list`
(repeated retained object), `bulk` (raw batches of4096). Choose an allowed CPU
instead of0 where necessary. `RAYFORCE_CORES=0` disables background helpers.
Build every compared archive with the same compiler/flags. Do not time alongside
builds or sanitizer tests. Run one discarded fresh process per variant/case,
then randomize five further fresh processes per variant/case and retain samples.
The project `make lib` optimized flags, not a debug/sanitizer archive, were used.

## Local observation, 2026-10-21

Ubuntu24/x86-64, i7-6700, Clang18, one pinned logical CPU, shared host and
uncontrolled frequency governor. Medians of five fresh processes after warm-up;
2,000,000 elements, preallocated/pre-touched buffers, full validation outside timing:

| Case | accepted646, ms | this change, ms |
|---|---:|---:|
| Single vector append |27.186|23.535|
| Single LIST append |15.343|12.065|
| Raw count1 |30.934|30.495|
| Raw batch4096 |0.919|0.869|

The raw paths are deliberately unchanged; their overlapping sample ranges do
**not** establish an improvement. This is a workload-specific result, not a
no-regression or general speedup guarantee. A synthetic accepted646 tree with
only #925's production changes reversed was also compared: it showed that the
ordinary-path work added by that change was measurable even without resizing.
It is not an accepted historical release or an alternative proposed fix.

A first experiment only moved arithmetic below the capacity branch. That helped
vector append but left LIST's enlarged register-save frame. Outlining the rare
geometry/allocation path removed that remaining common-path cost. The actual
allocation sizing policy and externally requested raw-count preflight remain.
