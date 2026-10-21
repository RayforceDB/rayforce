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
probe budgets one morsel (1024) of actual unequal-ID symbol comparisons,
rather than rows. Constant, low-transition, and bypassed secondary keys can
finish that probe without rank setup. A preceding key that already orders a
pair consumes no symbol budget. Early inversions still stop immediately.
Small tables and scans without secondary workers retain the serial scan. When ranks are unavailable because
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
and 200,000-row input with an inversion at the last pair. Under the current
adaptive probe, that low-transition late inversion is discovered without
preparing ranks. A separate high-transition sorted-prefix C regression
exercises the rank handoff and borrowed lifetime after the budget is spent. It also checks the
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

| Case (ascending) | Resolutions at eaff260a | At 8fcd0bc4 |
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

The uninstrumented ascending medians in milliseconds at the unsorted-input
revision `8fcd0bc4` were:

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

## Large constant and low-transition controls

`constant_work.c` reproduces the additional review case with three total
participants, 70,000 unused names interned before the used keys, and 65,537
rows. It checks ascending and descending sorts for a constant symbol key,
two long symbol runs, and a changing secondary symbol key bypassed by a
unique leading integer key. Every output key and row identity is validated.

Build it like `unsorted_work.c`, substituting `constant_work.c`. By default
it reports uninstrumented sort time. Adding `-DCOUNT_RESOLUTIONS` and
`-Wl,--wrap=ray_sym_domain_str` asserts exact lookup counts: zero, two, and
zero for those three cases. The previous `73f7c0ea` source fails these work
assertions while still returning correct rows.

| Shape, 65,537 rows | Upstream lookups | At 73f7c0ea | Adaptive probe |
|---|---:|---:|---:|
| Constant | 0 | 70003 | 0 |
| Two long runs | 2 | 70003 | 2 |
| Secondary key bypassed | 0 | 70003 | 0 |

Counts agree in both directions. The larger raw ID prefix in the review's
process produced 70,422 lookups; this fresh process has 70,003 prefix IDs.
The underlying issue and repaired work counts are the same.

Uninstrumented ascending medians over three processes after one warm-up:

| Shape | Upstream, ms | At 73f7c0ea, ms | Adaptive probe, ms |
|---|---:|---:|---:|
| Constant | 0.080 | 3.771 | 0.091 |
| Two long runs | 0.069 | 2.440 | 0.084 |
| Secondary key bypassed | 0.625 | 3.252 | 0.530 |

Other release objects were identical. The upstream and old sort objects
used clang `-O3 -march=native -std=c17`; the current object uses the release
Makefile flags. Inputs/validation are outside timing, cases run in fixed
order, and the host was shared. Submillisecond differences are not rankings.
The work-count checks, rather than the timings, pin the removal of setup.

The adaptive probe leaves the worker comparator, heap buffer ownership,
sparse fallback, partition route, and borrowed-rank handoff unchanged. The
single-outlier late-inversion work control now makes 60,007 resolutions:
four probe lookups plus one 60,003-ID real-sort rank build. Early, small and
serial counts remain 60,005, 28 and 60,005. The updated C stability test has
1024 actual symbol comparisons in its sorted prefix, then a late inversion,
so ASan/UBSan still exercises the handoff instead of accidentally bypassing
it. Constant, two-run and bypassed-key C tests cover the newly cheap route.

A contemporary dense-target control linked the `73f7c0ea` sort source with
matching release flags and otherwise identical objects. Three observations
after a warm-up, on the same shared host:

| Secondary workers | 73f7c0ea symbols, ms | Adaptive symbols, ms | 73f7c0ea integers, ms | Adaptive integers, ms |
|---:|---:|---:|---:|---:|
| 4 | 19.881 | 20.554 | 18.036 | 16.930 |
| 16 | 21.416 | 20.914 | 19.399 | 18.585 |

The parallel contention fix remains effective; small differences should not
be treated as a regression factor. Earlier measurements near 14 ms used a
different host load/CPU state, so this table compares the two revisions in
the current session instead of comparing only against historical timings.
