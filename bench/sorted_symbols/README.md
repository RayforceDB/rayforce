# Already-sorted symbol keys

Reproduce the sortedness-check contention in #933 without data files. The
script builds 2,630,400 rows ordered by `(timestamp, symbol)`, with 100
lexically ordered symbols per timestamp. Symbol creation order differs from
lexical order. Construction and correctness checks are outside the timed
first `xasc`; start a fresh process for each observation.

```sh
make -j8 release
RAYFORCE_CORES=4 ./rayforce bench/sorted_symbols/sorted_symbols.rfl
```

`RAYFORCE_CORES` requests secondary workers; zero is serial. Test 0, 1, 4
and 16. Change `(set numeric 0)` to `(set numeric 1)` in a copy of the script
for the lexical integer-rank control. Discard one warm-up process and take
the median of three fresh processes per setting and key type. Output includes
the timed milliseconds, full key/row-count validation, and build identity.
The small reversed-input control verifies actual sorting outside timing.

## Implementation

The early sortedness scan compares adjacent rows, so timestamp ties often
require a symbol comparison on every row. Resolving those symbol IDs inside
parallel workers takes the shared runtime symbol lock repeatedly. Preparing
lexical ranks with `build_enum_rank` before dispatch replaces those lookups
with integer comparisons for table sorts. When ranks are unavailable because
of the density budget or allocation failure, the check keeps the string
comparator and scans serially; unavailable ranks never mean "not sorted".
Integer-only keys retain their existing parallel scan. Null and type
admissibility gates are unchanged.

`ray_key_cols_sorted`, used by streaming parted ORDER BY, skips rank
preparation entirely. Symbol segments use the original equal-ID/string scan
serially, so constant segments need no string resolution and repeated small
segments do not repeatedly sort the domain. This also avoids shared-lock
contention. Table sorts still prepare call-local ranks and free them after
workers join. Their rank storage is sized through the maximum used intern
ID, and unsorted input may prepare those ranks again in the normal sort.
The serial fallback trades parallelism for bounded setup and no contention.
This change does not persist ordering ranks or change symbol representation.

## Observed timings

Measured on 2026-10-21 on a Ryzen AI 7 350, 16 logical CPUs, Linux, clang
optimized builds of upstream `64645998` and the patched source in the same
isolated build tree. Each cell is the median of three fresh processes after
one discarded warm-up. The machine was a shared desktop; small timing
differences are not decisive.

| Secondary workers | Before symbols, ms | After symbols, ms | Before integers, ms | After integers, ms |
|---:|---:|---:|---:|---:|
| 0 | 52.036 | 21.773 | 21.254 | 22.069 |
| 1 | 85.375 | 17.608 | 16.648 | 16.925 |
| 4 | 291.224 | 14.876 | 14.113 | 14.165 |
| 16 | 1227.420 | 14.496 | 13.487 | 14.071 |

Full reverse-sorted symbol input, reversed before timing and checked against
the same ascending expected output, measured 50.544/50.765 ms before/after
serially and 42.751/43.127 ms with four secondary workers. This is one
unsorted-input control, not an exhaustive regression study.

## Sparse IDs and repeated partitions

`work_count.c` counts symbol-domain string resolutions with the linker's
`--wrap` mechanism. It interns 70,000 unused names BEFORE the used symbol,
then checks an eight-row constant column and a 10,000-row constant column
twice each. The first exceeds the density budget; the second fits it.
The probe is serial and measures admission/work counts, not wall time.
Run from the repository root after `make release` (GNU-compatible linker):

```sh
clang -O3 -Iinclude -Isrc bench/sorted_symbols/work_count.c \
  $(find src -name '*.rel.o' ! -path 'src/app/main.rel.o') \
  -lm -lpthread -Wl,--wrap=ray_sym_domain_str -o /tmp/symbol-work-count
/tmp/symbol-work-count
```

At the original PR head `f62ca12d`, both eight-row checks returned false;
both 10,000-row checks returned true but each performed 70,002 resolutions.
With the fallback/partition fix, all four return true with zero resolutions.
The same probe source was linked with the before/after sort objects; other
release objects were unchanged. This is a focused instrumented control.

`parted_symbols.rfl` populates a database FILE symbol domain with 70,000
padding names, writes two 10,000-row constant-symbol partitions, then times
50 ascending queries over the mounted table. Setup is outside timing.
It checks every result count and requires exactly 100 streaming partition
runs, so a flat fallback is an error. Run sequentially: the script uses and
removes `/tmp/rayforce_bench_rank_parts`.

```sh
RAYFORCE_CORES=4 ./rayforce bench/sorted_symbols/parted_symbols.rfl
```

After one discarded warm-up, the median of three fresh processes was
232.272 ms on the original PR implementation and 1.766 ms after the fix,
for all 50 queries together. This reflects removal of repeated setup;
it is not a general query benchmark. The host was the same shared desktop.
The sparse eight-row streaming case is covered separately by
`test/rfl/sort/sorted_symbol_sparse_parted.rfl`, including exact streaming
counter deltas and full sequence checks in both budget regimes. It fails
on `f62ca12d` and passes after this fix.

A fresh run of the original 2,630,400-row benchmark after the fallback fix
retained the speedup (three measurements after one discarded warm-up):

| Secondary workers | Symbols, ms | Integer ranks, ms |
|---:|---:|---:|
| 0 | 20.307 | 19.572 |
| 1 | 17.948 | 15.820 |
| 4 | 13.793 | 13.001 |
| 16 | 12.842 | 12.076 |
