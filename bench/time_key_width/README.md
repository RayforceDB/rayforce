# `(SYM,TIME)` order-check cost

This native **internal** helper benchmark measures `ray_key_cols_sorted`, not
whole-query or full-sort latency. It constructs2M already-ordered rows in100
symbol groups before timing. TIME values use int32 cells; the I64 control uses
values outside int32 range. Result eligibility and both complete source buffers
are checked after timing. The pool is created before timing.

```sh
make -j4 lib
clang -std=c17 -O3 -march=native -DNDEBUG -Iinclude -Isrc \
  bench/time_key_width/scan.c librayforce.a -rdynamic -lpthread -lm -ldl \
  -o /tmp/time-key-check
RAYFORCE_CORES=0 taskset -c 0 /tmp/time-key-check time asc
RAYFORCE_CORES=0 taskset -c 0 /tmp/time-key-check i64 asc
RAYFORCE_CORES=0 taskset -c 0 /tmp/time-key-check time desc
RAYFORCE_CORES=3 taskset -c 0,1,2,3 /tmp/time-key-check time asc
```

Choose allowed logical CPUs on your host. The environment counts **background
helpers**, not total participants; actual worker participation is not measured.
Use the same compiler/flags/base on both sides and no overlapping builds/tests.

## Historical pre-#937 observation (`a9217925`)

The reference is the **correct generic fallback** (remove TIME from the i64
shortcut), not the old wrong-width kernel. Both reference and proposal include
accepted938's append changes. Ubuntu24/i7-6700/Clang18 project optimized flags;
one pinned logical CPU without helpers, first four allowed logical CPUs with3
helpers. Shared host/governor not isolated. One discarded fresh process per
variant/case, then five randomized fresh processes:

| TIME ascending,2M rows | correct generic, median ms | correct int32, median ms |
|---|---:|---:|
| no background helpers |16.086|3.775|
|3 background helpers |4.907|1.952|

Do not interpret these as speedups over the old wrong answer or as full-sort
improvements. Descending still uses generic comparison; no new descending lane
is claimed. An initial five-sample I64 control was noisy (3.460/4.947ms, proposed
range3.422–7.998). A **16-fresh-process unchanged-code repeat** gave3.562/3.490ms
with overlapping ranges. Retain that limitation rather than claiming a universal
no-regression result. The tests check semantics, not a flaky wall-time threshold.

## Integration with #937

The shared probe and worker now instantiate the same loop for TIME/int32 and
I64/TIMESTAMP/int64. The type choice remains outside rows; only unequal SYM
boundaries spend the rank-preparation comparison budget. The direct unranked
helper is serial after #937 even when background helpers are configured: do not
reuse the historical three-helper timings as current parallel performance.

A new matched Clang18/current-dev-D715 common archive comparison replaced only
the immutable sort object. TIME ascending remains compared against a **correct
generic** reference, never the incorrect 64-bit read. One discarded warm process
per cell followed by eight randomized fresh processes: TIME ascending14.502→
3.850ms; descending14.510→14.551ms with overlapping ranges. Both at zero helpers.
Whole-xasc I64 controls were22.643→22.939ms(1participant),22.552→22.914(4);
TIMESTAMP22.741→22.923 and22.578→22.827. All control ranges overlap; small
median increases remain disclosed rather than claiming universal neutrality.
There were108validated processes total(12warm/96measured). Output values and
source buffers were checked outside timing; requested participants do not prove
physical worker participation. This is a single-sort-TU/common-library study,
not a newly built whole-candidate release or an end-to-end ClickBench result.
