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
with integer comparisons. Both sortedness-check callers use the same helper;
integer-only keys retain their existing scan, and allocation failure declines
the shortcut. Null and type admissibility gates are unchanged.

The rank table is local to this call and is released after workers join.
It is sized through the maximum used intern ID, rather than just distinct IDs
in the column. This adds preparation and temporary memory for symbol keys;
if input is unsorted, the normal sort may prepare ranks again. Many tiny
partitions, high cardinality and large sparse symbol domains need separate
performance coverage. This change does not persist ordering ranks or change
symbol representation.

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
