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
with integer comparisons for large parallel table checks. First, an unranked
one-morsel probe rejects an early inversion without preparing ranks. Small
tables and scans without secondary workers retain the serial string scan. When ranks are unavailable because
of the density budget or allocation failure, the check keeps the string
comparator and scans serially; unavailable ranks never mean "not sorted".
Integer-only keys retain their existing parallel scan. Null and type
admissibility gates are unchanged.

`ray_key_cols_sorted`, used by streaming parted ORDER BY, skips rank
preparation entirely. Symbol segments use the original equal-ID/string scan
serially, so constant segments need no string resolution and repeated small
segments do not repeatedly sort the domain. This also avoids shared-lock
contention. Table sorts retain any prepared ranks after workers join. The
real sort borrows them after a late inversion, including recursive single-key
sorts, so successful maps are not built twice. The caller frees them after
the real sort returns, or immediately on the already-sorted shortcut. Rank
storage remains sized through the maximum used intern ID; unavailable maps
may be retried by the real sort.
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

A fresh run of the original 2,630,400-row benchmark after the unsorted-input
fix retained the parallel speedup (three measurements after one warm-up).
The serial case now deliberately uses the original string scan, like
upstream, rather than preparing ranks without secondary workers:

| Secondary workers | Symbols, ms | Integer ranks, ms |
|---:|---:|---:|
| 0 | 50.825 | 19.238 |
| 1 | 17.234 | 15.049 |
| 4 | 14.060 | 12.622 |
| 16 | 12.734 | 12.032 |

## Unsorted and small table controls

`unsorted_work.c` pads the runtime domain with 60,000 unused names before
creating the used keys. It exercises ascending and descending table sorts:
8-row unsorted input, 200,000-row input with an inversion at the first pair,
and 200,000-row input with an inversion at the last pair. The latter passes
the leading probe and exercises borrowed rank lifetime. It also checks the
large unsorted case without secondary workers. Every output key is checked.
The C regression additionally verifies stable row identities for these cases.

For uninstrumented timing, build after `make release`:

```sh
clang -O3 -march=native -std=c17 -Iinclude -Isrc \
  bench/sorted_symbols/unsorted_work.c \
  $(find src -name '*.rel.o' ! -path 'src/app/main.rel.o') \
  -lm -lpthread -o /tmp/unsorted-sort-time
/tmp/unsorted-sort-time
```

For string-resolution counts, add `-DCOUNT_RESOLUTIONS` and
`-Wl,--wrap=ray_sym_domain_str` to that command. Ignore timing from this
instrumented build. The probe initializes five total workers for the first
six cases, then one for the serial control.

| Case (ascending) | Resolutions at eaff260a | After unsorted fix |
|---|---:|---:|
| Small | 60029 | 28 |
| Early inversion | 120006 | 60005 |
| Late inversion | 120006 | 60003 |
| Serial | 120006 | 60005 |

Descending counts match the corresponding ascending cases. Early input
now pays for one real-sort rank build plus its two probe lookups; late input
prepares that map once and lends it to the real sort. Small input stays on
the comparison path without a dense LUT. Counts are measured over the entire
table sort, including its precheck, rather than just the precheck.

The uninstrumented ascending medians in milliseconds were:

| Case | Upstream 64645998 | eaff260a | After unsorted fix |
|---|---:|---:|---:|
| Small | 0.022 | 2.155 | 0.025 |
| Early inversion | 3.030 | 5.131 | 3.161 |
| Late inversion | 3.191 | 5.023 | 3.072 |
| Serial | 2.919 | 4.883 | 3.032 |

These are three fresh-process observations after one discarded warm-up per
build. Other release objects were identical; the upstream and eaff260a sort
objects were compiled separately with clang `-O3 -march=native -std=c17`.
The current sort object uses the release Makefile flags. Cases run in a fixed
order within each process, so the small ascending case includes first-sort
initialization and should not be compared with the later descending case.
Construction and validation are outside timing. The machine was a shared
desktop; these controls demonstrate removal of the extra work, not absence
of regressions for every input shape. Borrowed-rank ownership was checked
under ASan/UBSan with both serial and parallel sort tests.

## Unbounded key-count regression

The table caller stores key IDs, columns, rank pointers, rank scratch headers
and direction bytes in one checked, zeroed heap allocation. The sortedness
helper borrows the rank buffers directly and has no key-count-sized stack
arrays; the partition caller supplies no rank buffers at all. Allocation
failure returns an error before sorting, and the caller frees the block on
every exit, after releasing any prepared rank headers.

`test/rfl/sort/sort_many_repeated_keys.rfl` checks `xasc` and `xdesc` on tiny
symbol and integer tables with 300,000 repeated key names. The C regression
`sort/sorted_many_repeated_keys` checks the query/partition helper directly
with a heap-backed 300,000-entry column list. These all-tie inputs force the
checks to visit every key. A reduced-stack sanitizer run verifies stack use
is independent of the user-supplied key count:

```sh
(ulimit -s 2048; RAYFORCE_CORES=4 ./rayforce.test -f many_repeated_keys)
```
