# Compressed-columns P0 measurements and gate decision

## Machine and commit

- build commit a8a3e5e6 (the commit the release binaries were built at; branch feat/compressed-columns, worktree compress-p0; RESULTS.md and estimator changes are in later commits)
- 8 cores (`nproc`), 62 GB RAM (`free -g`), kernel 6.8.0-100-generic
- Store: local 10M splayed ClickBench table, /skull/clickbench/rfsplayed, 9 999 505 rows, 105 columns, symfile at `<db>/.sym`
- The 100M store was unavailable (user ruling); no remote machine was used. The submission's parted store (/skull/tmp/rayforce_tests/current) does not exist locally and was not run.
- Estimator runtime on the 10M store: 86 s, single run.

Note on indexes: the store has no separate index files; HASH indexes are attached to the column files. Five columns carry one (WatchID, ClientEventTime, FUniqID, RefererHash, URLHash). UserID has none in this store (the estimator was run with `--keep-index UserID`, as the brief says, so that scenario treats UserID's index as kept).

## Whole-table breakdown (10M)

```
breakdown rows=9999505 data=3599827376 index=1141598560 sym=1207249691 other=2312 total=5948677939
scenario today                      bytes=5948677939 size_ratio=1.000 score_factor=1.0000
scenario columns encoded            bytes=3216322795 size_ratio=1.850 score_factor=0.9404
scenario columns + sym fsst         bytes=2673652592 size_ratio=2.225 score_factor=0.9231
scenario columns + sym fsst + kept ix bytes=1532056784 size_ratio=3.883 score_factor=0.8731
```

The estimator's "kept ix" scenario keeps only the UserID index (none exists). The measured index use below says RefererHash and URLHash must be kept, which adds 194 915 360 + 195 932 480 bytes: 1 922 904 624 bytes, ratio 3.094, score factor 0.8932 (3.094^-0.1; the factors above follow ratio^-0.1).

## Per-type column ratios (sum of `col` lines; encoded includes null bitmaps and DICT)

| type | columns | raw bytes | encoded bytes | ratio | index bytes |
|---|---|---|---|---|---|
| I16 | 48 | 959 955 456 | 140 403 592 | 6.84 | 132 096 |
| I32 | 19 | 759 963 520 | 128 670 000 | 5.91 | 52 288 |
| I64 | 6 | 479 976 576 | 260 169 848 | 1.84 | 1 034 508 992 |
| SYM (ids) | 28 | 1 119 945 456 | 152 978 952 | 7.32 | 0 |
| TIMESTAMP | 3 | 239 988 288 | 184 686 800 | 1.30 | 106 902 432 |
| DATE | 1 | 39 998 080 | 563 040 | 71.04 | 2 752 |
| **all** | 105 | 3 599 827 376 | 867 472 232 | **4.150** | 1 141 598 560 |

Columns that barely compress (ratio below 1.5): WatchID 0.99, HID 1.03, RefererHash 1.03, URLHash 1.18, ClientEventTime 1.20, TIMESTAMP EventTime/LocalEventTime 1.35, RefererCategoryID 1.35, Referer 1.41, URL 1.47 (SYM id columns). Full `col` lines are in /tmp/ce_10m.txt (106 lines, not committed).

## Encoding mix (fraction of vectors, not of bytes)

```
RAW 0.0370  CONST 0.3960  FOR_BP 0.1639  DELTA_BP 0.0001  RLE 0.3836  DICT 0.0194
```

## Predicate groups (skip rates)

`undecided_bytes` is the encoded bytes of the vectors undecided under the combined decision of all predicates of the group (the earlier run summed a running, per-predicate state).

```
group g_adv  chunk_decided=0.0721 vector_decided=0.1840 undecided=0.7439 pred_bytes=793464 undecided_bytes=773448
group g_mpm  chunk_decided=0.0000 vector_decided=0.0725 undecided=0.9275 pred_bytes=1284168 undecided_bytes=1278504
group g_sp   chunk_decided=0.0000 vector_decided=0.0140 undecided=0.9860 pred_bytes=11463472 undecided_bytes=11462376
group g_url  chunk_decided=0.8296 vector_decided=0.1468 undecided=0.0236 pred_bytes=26816336 undecided_bytes=612656
group g_ref  chunk_decided=0.0000 vector_decided=0.0002 undecided=0.9998 pred_bytes=27909920 undecided_bytes=27905040
group g_user chunk_decided=0.7013 vector_decided=0.2745 undecided=0.0243 pred_bytes=16247048 undecided_bytes=1085416
group g_q37  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=1765176 undecided_bytes=174096
group g_q40  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=82436744 undecided_bytes=5214784
group g_q41  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=69231040 undecided_bytes=5070064
group g_q42  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=1765176 undecided_bytes=174096
```

All rows=9 999 505. The q37-q42 groups (CounterID == 62 plus a date range) are decided at chunk level for 92.8% of rows because the table is ordered by CounterID/EventDate. g_ref (Referer != 0) and g_sp/g_mpm/g_adv are mostly undecided: those predicates select a large share of rows.

## Symfile

```
sym file=1207249691 fsst=664579488 ratio=1.82 snapshot=1274980311 decode_s=3.544 count=8466329
```

FSST would shrink the symfile 1.82x. The decompressed snapshot is 1 274 980 311 bytes (1.056x the file); a single-thread decode of every block takes 3.544 s.

## Per-query index use (actual consults via `.idx.uses`, 10M store)

Queries 1-40 and 43: `{}`. Query 41: `{RefererHash:1}`. Query 42: `{URLHash:1}`.
Raw table is in /tmp/ce_iu_10m.tsv. `{}` means no index fast path was consulted; fused query shapes (for example `(count (select ...))`) never reach index sites and legitimately report `{}`.

Parted tables never consult segment indexes. The benchmark submission uses a parted load, so segment indexes added by the converter to parted databases are never used by the submission as it runs today; the use counts above describe a splayed load only.

## Extrapolation to 100M

Method: linear in row count (x10.0 for 9 999 505 -> 99 997 497 rows).

| item | 10M measured | x10.0 | spec 100M baseline | consistent? |
|---|---|---|---|---|
| column data raw | 3.60 GB | 36.0 GB | 36.0 GB | yes |
| column data encoded | 0.867 GB | 8.67 GB | 8.6 GB est. | yes (+1%) |
| ratio | 4.15x | | 4.1x | yes |
| HASH indexes | 1.14 GB | 11.4 GB | 11.7 GB | yes (-2.5%) |
| .sym | 1.207 GB | 12.1 GB (alt. 11.3 GB x 1.056 = 11.9 GB snapshot) | 11.3 GB | within 7% (symfile grows sub-linearly in distinct strings, not exactly linear; the 10M file is 1.07x of 1/10 of the 100M one) |

Caveats: encoded size depends on chunk-level locality, so per-chunk behaviour is assumed unchanged at 10x rows (same data ordering, same chunk size; 100M column files are only more chunks). Index size is the hash table, close to linear in rows. The symfile and snapshot depend on distinct-string count, which is not guaranteed linear. Skip rates are ratios and do not extrapolate by row count; they are only valid if the 100M table has the same ordering. Decode time is extrapolated at 33-35 s only if per-block cost is flat.

## Snapshot vs cold read, parallel decode, working set

```
sym file=1207249691 fsst=664579488 ratio=1.82 snapshot=1274980311 decode_s=3.570 sym_parallel_decode_s=0.667 threads=8 count=8466329
```

- Cold read: could not be measured. `sudo -n tee /proc/sys/vm/drop_caches` failed (password required); not retried. Warm read of the symfile: 0.119 s for 1.2 GB.
- Decode of all blocks: 3.57 s single thread (about 360 MB/s), 0.667 s on 8 threads (5.4x; blocks are independent, work-stealing over 130 blocks; threads = online CPUs, no knob).
- 100M snapshot figure used throughout: 10x the measured 10M snapshot = 12.7 GB. The alternative 11.3 GB x 1.056 = 11.9 GB (spec symfile size times the measured snapshot/file ratio) differs by 7%; the 10x figure is used because it is the conservative one. Parallel decode at 100M, if per-block cost is flat and cores are 8: about 6.7 s (single-thread about 36 s).
- Benchmark working set, measured on the 10M splayed store with the release ./rayforce (`-t 8`, one process, `.db.splayed.get` once, all 43 queries once, script built like run.sh splits queries.rfl; 43 results, no errors):
  - `/usr/bin/time -v`: Maximum resident set size 5 028 388 kB (4.8 GiB), wall 2.57 s, 0 major faults (page cache was warm).
  - `/proc/PID/status` polled every 20 ms: peak RssAnon 3 053 956 kB (heap, 2.9 GiB) and peak RssFile 2 009 884 kB (mmapped column pages, evictable). Sampled peaks are lower bounds and need not coincide.
  - The RssAnon peak includes today's symbol heap (offset table, atom array, touched atoms; the symfile itself is mmapped and counts in RssFile); it was not separated.
  - Assumption: x10 for 100M, heap about 30.5 GB (not measured at 100M; heap does not necessarily scale linearly, e.g. hash-group state depends on distinct keys). File-backed column pages are counted as evictable page cache, not heap.

## Re-run after the review fixes

The estimator was changed and re-run on the same store: the FSST `lengths` slot is sized with RAW or FOR_BP only (plus the 8-byte child header), the VALUE_DICT charge now includes the side table (32 B) and a 32-aligned entry, and `undecided_bytes` comes from the final combined decisions. Changed: column encoded total 867 470 818 -> 867 472 232 (+1 414 B; I16 +702, I32 +384, I64 +216, TIMESTAMP +112; ratios unchanged to two decimals); symfile fsst 664 513 344 -> 664 579 488 (+66 144 B, +0.010%, ratio 1.82 unchanged); the three scenario byte counts moved by +1 414, +67 558 and +67 558 B, and no ratio or score factor changed at the printed precision (the keep-list scenario is 1 922 904 624 B, ratio 3.094). `undecided_bytes` fell for g_q37 and g_q42 (177 216 -> 174 096), g_q40 (6 069 864 -> 5 214 784) and g_q41 (5 910 280 -> 5 070 064); the other groups did not change. Index use re-run (`index_use.sh`, 43 queries): unchanged, only query 41 `{RefererHash:1}` and query 42 `{URLHash:1}`. Decode times vary run to run (single-thread 3.509 s, 8 threads 0.658 s).

## Decisions

- **P0 gate: PASS.** Column data is 4.15x smaller (3.60 GB -> 0.867 GB at 10M, in line with the spec's 4.1x at 100M), well above the 1.5x threshold; the whole-table breakdown and projection (1.85x columns only, 2.225x with sym FSST, 3.09x with index trimming) and per-query index use are reported. Caveat: 100M was not measured, only extrapolated.
- **PI input:** the keep list (RefererHash 194.9 MB, used by q41; URLHash 195.9 MB, used by q42) applies to splayed deployments only. On the parted load the submission uses, no segment index is consulted, so every converter-added index (all five, 1.14 GB at 10M, about 11.4 GB at 100M) is droppable. On a splayed load, WatchID (508.4 MB), FUniqID (135.2 MB) and ClientEventTime (106.9 MB) were never consulted: 750.5 MB of 1 141.6 MB at 10M (66%), about 7.5 GB at 100M. Only 2 of 43 queries consult any index, once each. Predicate group names are 0-based: g_qN is 1-based query N+1 (g_q37..g_q42 are queries 38..43), so g_q40 (RefererHash predicate) is query 41 and g_q41 (URLHash predicate) is query 42, matching the two index consults.
- **PS gate input:** the constraint that decides is memory headroom on 32 GB, not open latency. Open latency is small: parallel decode 0.667 s at 10M, about 6.7 s at 100M (single thread about 36 s). Memory: a full snapshot is a net heap addition of about 12.7 GB at 100M (minus the small offset table and atom array the engine already keeps, since the FILE symbol domain mmaps the symfile as page cache and materializes atoms lazily), on top of a heap extrapolated to about 30.5 GB (10M RssAnon peak 3.05 GB, x10 assumption). Today's symbol heap is not separated from that RssAnon peak. 30.5 + 12.7 is about 43 GB, far above 32 GB. The extrapolated heap leaves only about 1.5 GB headroom on 32 GB regardless of the PS choice. Encoded columns (8.67 GB) are file-backed and evictable so they do not count as heap. Recommendation: block cache (decode symbols on demand, bounded memory); the memory numbers strengthen it, and a full snapshot is not recommended on a 32 GB machine. PS steady state: by the spec's Bound rule a domain holds at most one decompressed snapshot (built at first open, never rebuilt for append flushes or conversions), so repeated appends and conversions do not grow it; only appended atoms add memory.

## P1 measured sizes

Release build, `bench/compress_estimate/p1_size.sh`: the 10M ClickBench splayed store (`.db.splayed.get`, 9 999 505 rows, 105 columns) re-saved with `{compress: true}` into a scratch directory, then per-column file sizes compared with the P0 estimator (`build/compress_estimate --db ... --keep-index UserID`, `col` lines: encoded bytes) on the same store. Size only, no timings. The scratch copy lives outside the repo and was removed after recording.

Generations observed: 100 columns generation 2; WatchID, ClientEventTime, FUniqID, RefererHash and URLHash generation 0 and byte-identical to the source (588 420 000, 186 893 024, 215 227 840, 274 911 456, 275 928 576 B). This matches the expectation: a persisted HASH index keeps the column generation 0 until P4.

| Group | Columns | gen-0 bytes | P1 bytes | P0 estimate | P1 / estimate | gen-0 / P1 | share of table column bytes |
|---|---|---|---|---|---|---|---|
| all generation-2 | 100 | 3 200 045 040 | 558 495 832 | 558 163 872 | 1.0006 | 5.73x | 67.5% |
| I16 | 48 | 960 087 552 | 140 403 584 | 140 403 592 | 1.0000 | 6.84x | 20.2% |
| I32 | 19 | 760 015 808 | 128 669 880 | 128 670 000 | 1.0000 | 5.91x | 16.0% |
| I64 | 2 | 159 997 696 | 17 323 136 | 17 323 128 | 1.0000 | 9.24x | 3.4% |
| SYM | 28 | 1 119 945 456 | 153 311 040 | 152 978 952 | 1.0022 | 7.31x | 23.6% |
| TIMESTAMP | 2 | 159 997 696 | 118 225 152 | 118 225 160 | 1.0000 | 1.35x | 3.4% |
| DATE | 1 | 40 000 832 | 563 040 | 563 040 | 1.0000 | 71.0x | 0.8% |

The five largest per-column deltas (P1 minus estimate) are all small: UserAgentMinor +3 739 824 B (the whole SYM excess is 332 088 B net of savings elsewhere), Referer +832 320, SearchPhrase +166 624, OriginalURL +86 264, FlashMinor2 +73 304; every other column is within 16 B.

Whole table (all files including `.sym` and `.d`, `du -sb`): generation 0 5 948 677 939 B, P1 3 307 128 731 B (1.80x). The `.sym` file is unchanged (1 207 249 691 B; symbol-file compression is not part of P1). The columns that stay generation 0 carry the inline HASH indexes (1.14 GB), so the 1.80x is the P1 floor before index policy (PI) and symfile work.

Extrapolation to 100M (rows x10, same ordering and chunk behaviour assumed, `.sym` at the earlier 10x figure): gen-0 table about 59.5 GB, P1 table about 33.1 GB (generation-2 column data 5.6 GB, generation-0 indexed columns 15.4 GB, `.sym` 12.1 GB). Generation-2 columns alone: 32.0 GB -> 5.6 GB.

## P1 gate decision

**P1 size gate (size per P0): PASS.** Overall P1 / P0 estimate = 1.0006 (limit 1.10); the worst type with at least 1% of the table is SYM at 1.0022; generation-2 column data is 5.73x smaller than generation 0 (limit 1.5x).

## PI index policy (10M store, release build, commit 2dcb78ae)

Policy, fixed in the plan before measuring: converters stop persisting
auto-built HASH indexes; explicit `.idx.hash` is always honoured; parted
converters never auto-build segment HASH indexes; q41 / q42 stay unchanged
(identical results, warm within 5%), with an in-memory HASH built on first
use if needed.

Stores: both converted from /skull/clickbench/hits_h.csv into
/home/hetoku/data/tmp/pi_bench (same filesystem), `conv_hash` by binary built
from detached copy of commit 2dcb78ae, `conv_nohash` by a second detached copy
of the same commit with the upgrade check returning 0 (measurement-only build,
not committed). Warm: 5 alternated rounds x 10 runs after a warm-up run in
each process. Cold: 5 runs, fresh process per run, page cache evicted before
each run by per-file POSIX_FADV_DONTNEED (dd iflag=nocache). Index use
verified with `.idx.uses`: conv_hash `{RefererHash:1}` / `{URLHash:1}`,
conv_nohash `{}` / `{}`. Result rows of q41 and q42 are identical across the
two stores (each store is also stable against itself).

| query | warm hash ms | warm no-hash ms | warm ratio | cold hash ms | cold no-hash ms | cold ratio |
|---|---|---|---|---|---|---|
| q41 | 5.849 | 9.154 | 1.565 | 74.418 | 218.279 | 2.933 |
| q42 | 4.522 | 7.975 | 1.763 | 37.176 | 180.057 | 4.843 |

Load (`.db.splayed.get`, cold): hash 3296.8 / 3301.1 ms (q41 / q42 runs),
no-hash 3272.1 / 3262.8 ms.

| converter | wall s | peak RSS | store bytes |
|---|---|---|---|
| with upgrade | 16.57 | 5 356 888 kB | 5 948 945 203 |
| without upgrade | 14.74 | 4 492 028 kB | 4 807 577 107 |

Per column (bytes with / without): WatchID 588 420 000 / 80 002 560,
ClientEventTime 186 893 024 / 80 002 560, FUniqID 215 227 840 / 80 002 560,
RefererHash 274 911 456 / 80 002 560, URLHash 275 928 576 / 80 002 560,
UserID 80 002 560 / 80 002 560 (no index in this store).

Extrapolated to 100M (x 10.0): index bytes saved 11.4 GB, converter time saved 18.3 s.

Informational (ruling C1), generation-2 store: `conv_gen2` = the no-hash
converter with `{compress: true}`; 25.97 s, peak RSS 4 492 372 kB, 2 074 724 107
bytes (whole table); RefererHash 77 548 464 B, URLHash 68 057 128 B; both
queries consult no index (`{}`). q41 / q42 warm, alternated with the other two
stores (5 x 10 runs): hash 5.785 / 4.521 ms, no-hash 9.200 / 7.998 ms
(ratio 1.590 / 1.769), gen2 9.227 / 7.958 ms (ratio 1.595 / 1.760). A
generation-2 store therefore shows the same gap as the gen-0 no-hash store.
Parted loads never consult segment indexes (P0), so the submission path is
unaffected by PI; the driver measures splayed stores only. A resaved table
that already carries a HASH keeps it.

**PI lazy build needed: yes** — q41 warm ratio 1.565, q42 warm ratio 1.763 (yes iff either exceeds 1.05).

### PI index policy — gate (head 564368c6 vs dev)

Script: `bench/compress_estimate/pi_gate.sh` (conversion, clause 1, measurement,
result identity) and `pi_gate.py` (tables and the gate line, printed from the
data, never typed). Head binary: release build of committed 564368c6 in a
detached copy; baseline: the dev release binary on a store its own converter
wrote (5 columns carry a persisted HASH: WatchID, ClientEventTime, FUniqID,
RefererHash, URLHash). Load kinds: splayed gen-0 (head store vs dev store),
splayed gen-2 (`{compress: true}`; dev has no generation 2, so the baseline
is the dev gen-0 store), parted (`.csv.parted`, 1 000 000 rows per
partition, head store vs dev store; both 4 807 844 987 B). All numbers come
from `.db.splayed.get` / `.db.parted.get` loads, release builds, 10M-row
ClickBench, all 43 queries. One fresh process per (kind, variant, query, round)
loads the store and runs the query 6 times (`timeit`, ms), head and baseline
alternated, order swapped every round, 5 rounds. First run = run 1 (fresh
process, warm page cache; median of rounds). 3-run total = runs 1..3 (median
of rounds). Warm = median of runs 2..6, best of the 5 rounds. A query is
"outside" when best-of-5 ratio > 1.05, the gap is at least 0.1 ms and the
median-of-rounds ratio is also > 1.05 (a 1 ms query moves 15% on jitter
alone); those over 1.05 but unconfirmed are listed as noise. Queries whose
best-of-5 was outside in the 5-round run (q25 gen-0; q20, q39 parted) were
re-measured with 15 rounds (confirm run replaces their rows): q25 1.056
(gap 0.05 ms), q20 1.045, q39 1.048: within 5%. No cold run: the gate
asks for warm, first run and 3-run totals. Result text of every query is
identical head vs baseline on all three kinds (and head is stable against
itself).

Converter (10M rows): gen-0 head 14.71 s, dev 16.23 s; store 4 807 577 107 B
(head) vs 5 948 945 203 B (dev) — byte count equal to Task 1's `nohash`;
gen-2 head 25.06 s, 2 074 724 107 B; parted head 27.38 s / dev 26.81 s,
equal size (parted never persisted hashes).
Clause 1: HASH columns on a fresh head conversion: gen-0 0, gen-2 0
(dev gen-0: 5).

q41 / q42 (the recorded deviation, ms head vs dev): splayed gen-0 warm
9.24 / 8.02 vs 5.87 / 4.66 (1.57x / 1.72x), first run 16.4 / 14.5 vs 13.5 / 11.0,
3-run 35.5 / 30.9 vs 25.7 / 20.5; gen-2 warm 1.57x / 1.73x (first run 86 / 82
ms, decode); parted warm 17.6 / 13.4 vs 25.0 / 13.1 (0.70x / 1.03x), first run
39.1 / 24.4 vs 111.7 / 23.6: unchanged or better, parted never consults segment
indexes. Spec deviation: on splayed loads the lazy hash is built only after
accumulated scan work reaches its build cost, so q41/q42 run at scan speed.

Findings other than the recorded q41/q42 deviation:
- splayed gen-2 first run is slower on every query (first-run total 7 740 ms
  vs 2 586 ms, 3-run total 11 395 vs 6 228 ms, ratio 2.99 / 1.83): first
  touch of a compressed column decodes it (e.g. q3 0.02 -> 18 ms); warm is
  equal (1.005). Not index related; it is the cost of the compressed format
  on the first query of a process.
- parted q16 (`group by UserID`) first run 145 ms vs 83 ms (1.73x, repeatable
  in 15 rounds, 3-run 298 vs 233 ms); warm 1.03. Parted loads consult no
  segment index, so this is not the index policy; not investigated here.
- q25 gen-0 and q20/q39 parted: noise (see above).

```
PI GATE: clause1 PASS ; clause2 splayed-gen0 FAIL (q41 1.6x, q42 1.7x warm; deviation recorded) ; clause2 splayed-gen2 FAIL (q41 1.6x, q42 1.7x warm; deviation recorded) ; clause2 parted PASS ; first run over 1.05x (informational): splayed-gen0 q41 q42; splayed-gen2 42 queries; parted q16
```

Per-query tables (ms; ratio = head / baseline; ** = outside):
## splayed-gen0 (ms; head vs baseline; ratio = head/baseline)
| q | warm head | warm base | ratio | first head | first base | ratio | 3-run head | 3-run base | ratio |
|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.000 | 0.000 | 0.995 | 0.010 | 0.006 | 1.698 | 0.011 | 0.007 | 1.622 |
| q2 | 1.122 | 1.116 | 1.006 | 1.976 | 2.012 | 0.982 | 4.300 | 4.337 | 0.991 |
| q3 | 0.002 | 0.002 | 1.000 | 0.022 | 0.021 | 1.040 | 0.027 | 0.026 | 1.032 |
| q4 | 0.000 | 0.000 | 1.003 | 0.012 | 0.012 | 1.057 | 0.014 | 0.013 | 1.066 |
| q5 | 0.000 | 0.000 | 0.947 | 0.025 | 0.025 | 1.016 | 0.027 | 0.026 | 1.011 |
| q6 | 0.000 | 0.000 | 0.868 | 0.026 | 0.025 | 1.006 | 0.027 | 0.027 | 1.010 |
| q7 | 0.002 | 0.002 | 1.098 | 0.021 | 0.020 | 1.042 | 0.025 | 0.024 | 1.043 |
| q8 | 3.530 | 3.495 | 1.010 | 8.897 | 8.799 | 1.011 | 15.975 | 15.915 | 1.004 |
| q9 | 76.181 | 75.342 | 1.011 | 112.969 | 114.727 | 0.985 | 265.734 | 268.238 | 0.991 |
| q10 | 69.079 | 68.858 | 1.003 | 96.688 | 96.721 | 1.000 | 234.371 | 236.033 | 0.993 |
| q11 | 16.728 | 16.721 | 1.000 | 25.733 | 25.496 | 1.009 | 59.603 | 59.112 | 1.008 |
| q12 | 17.054 | 17.353 | 0.983 | 26.074 | 26.755 | 0.975 | 60.702 | 61.823 | 0.982 |
| q13 | 31.892 | 31.816 | 1.002 | 41.139 | 41.463 | 0.992 | 105.888 | 105.371 | 1.005 |
| q14 | 63.077 | 62.979 | 1.002 | 87.680 | 88.353 | 0.992 | 217.947 | 217.207 | 1.003 |
| q15 | 36.405 | 36.683 | 0.992 | 47.817 | 47.912 | 0.998 | 122.266 | 123.942 | 0.986 |
| q16 | 69.582 | 69.670 | 0.999 | 94.216 | 94.616 | 0.996 | 233.948 | 235.049 | 0.995 |
| q17 | 92.985 | 93.118 | 0.999 | 130.100 | 130.108 | 1.000 | 317.985 | 317.252 | 1.002 |
| q18 | 6.915 | 6.888 | 1.004 | 13.987 | 15.355 | 0.911 | 28.000 | 29.431 | 0.951 |
| q19 | 136.934 | 137.750 | 0.994 | 210.199 | 215.739 | 0.974 | 489.458 | 497.173 | 0.984 |
| q20 | 1.141 | 1.133 | 1.007 | 6.881 | 6.955 | 0.989 | 9.484 | 9.483 | 1.000 |
| q21 | 42.061 | 42.102 | 0.999 | 45.316 | 45.852 | 0.988 | 130.470 | 130.485 | 1.000 |
| q22 | 27.590 | 27.508 | 1.003 | 32.153 | 32.919 | 0.977 | 87.932 | 88.794 | 0.990 |
| q23 | 48.545 | 48.594 | 0.999 | 60.794 | 61.102 | 0.995 | 159.105 | 159.075 | 1.000 |
| q24 | 29.261 | 29.066 | 1.007 | 35.008 | 35.072 | 0.998 | 94.348 | 95.043 | 0.993 |
| q25 | 0.897 | 0.849 | 1.056 | 4.364 | 4.754 | 0.918 | 6.430 | 6.695 | 0.960 |
| q26 | 10.729 | 10.822 | 0.991 | 15.653 | 16.204 | 0.966 | 37.354 | 37.970 | 0.984 |
| q27 | 1.029 | 0.971 | 1.059 | 4.325 | 4.769 | 0.907 | 6.467 | 6.932 | 0.933 |
| q28 | 83.354 | 82.885 | 1.006 | 86.116 | 85.224 | 1.010 | 253.769 | 253.914 | 0.999 |
| q29 | 351.394 | 343.020 | 1.024 | 440.875 | 431.198 | 1.022 | 1153.210 | 1126.725 | 1.024 |
| q30 | 0.031 | 0.019 | 1.692 | 0.035 | 0.037 | 0.960 | 0.103 | 0.089 | 1.160 |
| q31 | 52.473 | 54.099 | 0.970 | 70.471 | 71.560 | 0.985 | 176.617 | 180.212 | 0.980 |
| q32 | 59.194 | 60.634 | 0.976 | 85.282 | 87.570 | 0.974 | 204.558 | 209.638 | 0.976 |
| q33 | 202.209 | 202.702 | 0.998 | 360.525 | 365.592 | 0.986 | 770.456 | 778.730 | 0.989 |
| q34 | 39.495 | 39.530 | 0.999 | 70.745 | 70.986 | 0.997 | 149.712 | 149.923 | 0.999 |
| q35 | 39.341 | 39.303 | 1.001 | 70.401 | 70.786 | 0.995 | 149.347 | 149.462 | 0.999 |
| q36 | 64.032 | 63.760 | 1.004 | 87.474 | 88.006 | 0.994 | 215.936 | 216.307 | 0.998 |
| q37 | 20.501 | 20.664 | 0.992 | 30.512 | 30.978 | 0.985 | 72.612 | 72.627 | 1.000 |
| q38 | 20.284 | 19.709 | 1.029 | 30.081 | 29.332 | 1.026 | 75.577 | 74.652 | 1.012 |
| q39 | 7.417 | 7.501 | 0.989 | 14.921 | 15.013 | 0.994 | 30.166 | 30.320 | 0.995 |
| q40 | 58.657 | 60.025 | 0.977 | 90.979 | 91.792 | 0.991 | 210.184 | 213.638 | 0.984 |
| q41 | 9.238 | 5.873 | 1.573 ** | 16.404 | 13.505 | 1.215 | 35.495 | 25.660 | 1.383 |
| q42 | 8.021 | 4.664 | 1.720 ** | 14.451 | 10.985 | 1.316 | 30.934 | 20.523 | 1.507 |
| q43 | 9.214 | 10.038 | 0.918 | 18.603 | 19.536 | 0.952 | 37.948 | 40.487 | 0.937 |
| total | 1807.6 | 1797.3 | 1.006 | 2590.0 | 2597.9 | 0.997 | 6254.5 | 6248.4 | 1.001 |
warm outside 1.05x (gap >= 0.1 ms): [('q41', 1.573), ('q42', 1.72)]
warm over 1.05x but not confirmed (gap < 0.1 ms or median-of-rounds ratio <= 1.05: noise): [('q7', 1.098), ('q25', 1.056), ('q27', 1.059), ('q30', 1.692)]
first run outside 1.05x (gap >= 0.1 ms): [('q41', 1.215), ('q42', 1.316)]
result text differs head vs baseline: stable-head diffs []; head unstable against itself (tie order) []

## splayed-gen2 (ms; head vs baseline; ratio = head/baseline)
| q | warm head | warm base | ratio | first head | first base | ratio | 3-run head | 3-run base | ratio |
|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.000 | 0.000 | 0.964 | 0.010 | 0.010 | 1.014 | 0.011 | 0.011 | 1.016 |
| q2 | 1.104 | 1.121 | 0.985 | 8.039 | 1.987 | 4.045 | 10.403 | 4.299 | 2.420 |
| q3 | 0.002 | 0.002 | 1.061 | 16.404 | 0.018 | 924.342 | 16.413 | 0.023 | 725.418 |
| q4 | 0.000 | 0.000 | 1.083 | 16.021 | 0.015 | 1040.824 | 16.024 | 0.017 | 970.616 |
| q5 | 0.001 | 0.000 | 1.333 | 16.420 | 0.025 | 667.112 | 16.424 | 0.026 | 631.219 |
| q6 | 0.000 | 0.000 | 1.080 | 15.530 | 0.025 | 630.336 | 15.533 | 0.026 | 594.766 |
| q7 | 0.001 | 0.002 | 0.894 | 6.240 | 0.020 | 310.868 | 6.244 | 0.024 | 256.419 |
| q8 | 3.512 | 3.523 | 0.997 | 14.850 | 9.006 | 1.649 | 21.999 | 16.224 | 1.356 |
| q9 | 75.863 | 75.058 | 1.011 | 133.561 | 113.886 | 1.173 | 287.482 | 266.015 | 1.081 |
| q10 | 68.559 | 68.151 | 1.006 | 137.883 | 96.636 | 1.427 | 275.705 | 235.166 | 1.172 |
| q11 | 16.757 | 16.706 | 1.003 | 49.883 | 25.638 | 1.946 | 83.839 | 59.410 | 1.411 |
| q12 | 17.157 | 17.336 | 0.990 | 56.519 | 26.398 | 2.141 | 91.613 | 61.785 | 1.483 |
| q13 | 31.921 | 31.887 | 1.001 | 56.916 | 41.234 | 1.380 | 122.126 | 106.666 | 1.145 |
| q14 | 62.836 | 63.276 | 0.993 | 117.868 | 88.924 | 1.325 | 246.660 | 217.423 | 1.134 |
| q15 | 36.555 | 36.344 | 1.006 | 71.562 | 47.824 | 1.496 | 145.571 | 122.149 | 1.192 |
| q16 | 69.706 | 69.872 | 0.998 | 109.395 | 94.294 | 1.160 | 249.223 | 234.507 | 1.063 |
| q17 | 92.708 | 92.969 | 0.997 | 160.109 | 130.087 | 1.231 | 346.961 | 316.677 | 1.096 |
| q18 | 6.924 | 6.847 | 1.011 | 38.133 | 15.057 | 2.533 | 52.550 | 28.966 | 1.814 |
| q19 | 137.279 | 137.730 | 0.997 | 258.030 | 209.278 | 1.233 | 536.660 | 490.246 | 1.095 |
| q20 | 1.143 | 1.166 | 0.980 | 22.558 | 6.741 | 3.347 | 25.155 | 9.359 | 2.688 |
| q21 | 41.928 | 42.038 | 0.997 | 61.725 | 46.995 | 1.313 | 145.674 | 133.471 | 1.091 |
| q22 | 27.498 | 27.541 | 0.998 | 78.190 | 32.041 | 2.440 | 133.508 | 87.356 | 1.528 |
| q23 | 48.661 | 48.790 | 0.997 | 122.894 | 60.647 | 2.026 | 220.996 | 158.771 | 1.392 |
| q24 | 29.021 | 29.044 | 0.999 | 1029.160 | 35.044 | 29.368 | 1087.650 | 93.295 | 11.658 |
| q25 | 0.888 | 0.967 | 0.918 | 1001.600 | 4.728 | 211.853 | 1003.839 | 6.622 | 151.589 |
| q26 | 10.745 | 10.842 | 0.991 | 1005.570 | 16.489 | 60.986 | 1027.331 | 38.339 | 26.796 |
| q27 | 0.896 | 0.961 | 0.932 | 995.396 | 4.739 | 210.045 | 997.483 | 6.682 | 149.277 |
| q28 | 82.987 | 82.819 | 1.002 | 108.232 | 85.585 | 1.265 | 275.128 | 251.898 | 1.092 |
| q29 | 351.880 | 343.977 | 1.023 | 458.004 | 430.144 | 1.065 | 1166.497 | 1123.256 | 1.038 |
| q30 | 0.019 | 0.019 | 1.000 | 10.440 | 0.036 | 286.173 | 10.513 | 0.100 | 104.953 |
| q31 | 52.867 | 54.150 | 0.976 | 122.678 | 71.107 | 1.725 | 230.587 | 180.355 | 1.279 |
| q32 | 59.079 | 60.640 | 0.974 | 146.874 | 86.700 | 1.694 | 266.680 | 210.969 | 1.264 |
| q33 | 201.737 | 202.704 | 0.995 | 406.545 | 365.183 | 1.113 | 814.983 | 775.678 | 1.051 |
| q34 | 39.386 | 39.291 | 1.002 | 86.553 | 70.668 | 1.225 | 165.755 | 149.809 | 1.106 |
| q35 | 39.354 | 39.354 | 1.000 | 86.359 | 70.292 | 1.229 | 165.070 | 149.801 | 1.102 |
| q36 | 63.575 | 63.701 | 0.998 | 99.095 | 87.737 | 1.129 | 227.293 | 215.882 | 1.053 |
| q37 | 20.657 | 20.713 | 0.997 | 74.320 | 30.524 | 2.435 | 118.112 | 72.365 | 1.632 |
| q38 | 19.702 | 19.614 | 1.005 | 76.263 | 29.939 | 2.547 | 118.735 | 75.052 | 1.582 |
| q39 | 7.483 | 7.486 | 1.000 | 62.670 | 14.878 | 4.212 | 78.000 | 30.122 | 2.589 |
| q40 | 58.813 | 59.841 | 0.983 | 169.767 | 91.258 | 1.860 | 288.556 | 212.414 | 1.358 |
| q41 | 9.114 | 5.794 | 1.573 ** | 86.463 | 13.239 | 6.531 | 105.528 | 25.505 | 4.138 |
| q42 | 7.955 | 4.600 | 1.729 ** | 81.636 | 11.268 | 7.245 | 97.989 | 20.789 | 4.713 |
| q43 | 9.256 | 10.101 | 0.916 | 63.617 | 19.562 | 3.252 | 82.567 | 40.387 | 2.044 |
| total | 1805.5 | 1797.0 | 1.005 | 7740.0 | 2585.9 | 2.993 | 11395.1 | 6227.9 | 1.830 |
warm outside 1.05x (gap >= 0.1 ms): [('q41', 1.573), ('q42', 1.729)]
warm over 1.05x but not confirmed (gap < 0.1 ms or median-of-rounds ratio <= 1.05: noise): [('q3', 1.061), ('q4', 1.083), ('q5', 1.333), ('q6', 1.08)]
first run outside 1.05x (gap >= 0.1 ms): [('q2', 4.045), ('q3', 924.342), ('q4', 1040.824), ('q5', 667.112), ('q6', 630.336), ('q7', 310.868), ('q8', 1.649), ('q9', 1.173), ('q10', 1.427), ('q11', 1.946), ('q12', 2.141), ('q13', 1.38), ('q14', 1.325), ('q15', 1.496), ('q16', 1.16), ('q17', 1.231), ('q18', 2.533), ('q19', 1.233), ('q20', 3.347), ('q21', 1.313), ('q22', 2.44), ('q23', 2.026), ('q24', 29.368), ('q25', 211.853), ('q26', 60.986), ('q27', 210.045), ('q28', 1.265), ('q29', 1.065), ('q30', 286.173), ('q31', 1.725), ('q32', 1.694), ('q33', 1.113), ('q34', 1.225), ('q35', 1.229), ('q36', 1.129), ('q37', 2.435), ('q38', 2.547), ('q39', 4.212), ('q40', 1.86), ('q41', 6.531), ('q42', 7.245), ('q43', 3.252)]
result text differs head vs baseline: stable-head diffs []; head unstable against itself (tie order) []

## parted (ms; head vs baseline; ratio = head/baseline)
| q | warm head | warm base | ratio | first head | first base | ratio | 3-run head | 3-run base | ratio |
|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.000 | 0.000 | 1.036 | 0.010 | 0.006 | 1.711 | 0.010 | 0.006 | 1.630 |
| q2 | 6.548 | 6.565 | 0.997 | 17.239 | 17.080 | 1.009 | 30.756 | 30.545 | 1.007 |
| q3 | 534.530 | 531.901 | 1.005 | 547.609 | 555.732 | 0.985 | 1630.568 | 1625.175 | 1.003 |
| q4 | 274.737 | 274.666 | 1.000 | 324.076 | 317.029 | 1.022 | 874.750 | 873.829 | 1.001 |
| q5 | 273.886 | 275.156 | 0.995 | 321.902 | 316.661 | 1.017 | 874.392 | 871.353 | 1.003 |
| q6 | 339.239 | 354.685 | 0.956 | 9589.120 | 9617.410 | 0.997 | 10274.543 | 10326.471 | 0.995 |
| q7 | 523.284 | 528.049 | 0.991 | 557.477 | 550.751 | 1.012 | 1608.479 | 1611.945 | 0.998 |
| q8 | 9.146 | 8.898 | 1.028 | 21.352 | 21.233 | 1.006 | 39.872 | 39.093 | 1.020 |
| q9 | 89.326 | 89.335 | 1.000 | 176.321 | 176.611 | 0.998 | 357.533 | 356.988 | 1.002 |
| q10 | 86.757 | 86.833 | 0.999 | 202.315 | 202.819 | 0.998 | 376.433 | 377.952 | 0.996 |
| q11 | 355.919 | 358.032 | 0.994 | 582.423 | 604.809 | 0.963 | 1297.609 | 1324.438 | 0.980 |
| q12 | 358.864 | 356.961 | 1.005 | 579.782 | 599.664 | 0.967 | 1297.504 | 1319.477 | 0.983 |
| q13 | 40.126 | 39.998 | 1.003 | 70.378 | 70.981 | 0.992 | 151.174 | 152.093 | 0.994 |
| q14 | 1126.650 | 1127.240 | 0.999 | 1415.890 | 1425.640 | 0.993 | 3682.500 | 3687.260 | 0.999 |
| q15 | 46.840 | 45.988 | 1.019 | 89.537 | 89.308 | 1.003 | 184.014 | 183.677 | 1.002 |
| q16 | 75.918 | 73.629 | 1.031 | 144.402 | 83.517 | 1.729 | 298.099 | 233.077 | 1.279 |
| q17 | 103.211 | 103.607 | 0.996 | 206.068 | 205.331 | 1.004 | 413.859 | 414.037 | 1.000 |
| q18 | 17.133 | 139.177 | 0.123 | 83.448 | 248.389 | 0.336 | 117.880 | 527.614 | 0.223 |
| q19 | 140.647 | 140.954 | 0.998 | 337.261 | 336.401 | 1.003 | 621.365 | 621.529 | 1.000 |
| q20 | 4.593 | 4.396 | 1.045 | 14.455 | 14.302 | 1.011 | 24.045 | 23.429 | 1.026 |
| q21 | 41.742 | 41.667 | 1.002 | 46.651 | 46.631 | 1.000 | 130.396 | 130.175 | 1.002 |
| q22 | 60.613 | 60.213 | 1.007 | 87.920 | 86.796 | 1.013 | 212.318 | 209.176 | 1.015 |
| q23 | 93.997 | 93.921 | 1.001 | 210.035 | 220.817 | 0.951 | 399.208 | 410.527 | 0.972 |
| q24 | 44.564 | 44.339 | 1.005 | 132.708 | 140.459 | 0.945 | 222.322 | 229.776 | 0.968 |
| q25 | 1063.330 | 1065.000 | 0.998 | 1324.740 | 1342.400 | 0.987 | 3459.250 | 3481.860 | 0.994 |
| q26 | 1120.870 | 1117.680 | 1.003 | 1471.470 | 1492.130 | 0.986 | 3729.920 | 3732.890 | 0.999 |
| q27 | 1079.460 | 1078.210 | 1.001 | 1337.470 | 1360.150 | 0.983 | 3507.540 | 3528.450 | 0.994 |
| q28 | 91.553 | 91.461 | 1.001 | 147.442 | 148.620 | 0.992 | 331.971 | 332.989 | 0.997 |
| q29 | 360.752 | 350.821 | 1.028 | 468.611 | 461.795 | 1.015 | 1196.195 | 1171.096 | 1.021 |
| q30 | 266.654 | 263.339 | 1.013 | 280.969 | 276.262 | 1.017 | 829.220 | 817.072 | 1.015 |
| q31 | 55.143 | 54.953 | 1.003 | 125.212 | 125.744 | 0.996 | 238.528 | 237.626 | 1.004 |
| q32 | 69.225 | 68.844 | 1.006 | 178.718 | 178.417 | 1.002 | 317.722 | 320.151 | 0.992 |
| q33 | 216.283 | 215.747 | 1.002 | 462.433 | 461.041 | 1.003 | 900.791 | 897.351 | 1.004 |
| q34 | 43.201 | 43.163 | 1.001 | 96.547 | 96.399 | 1.002 | 184.860 | 183.463 | 1.008 |
| q35 | 387.736 | 385.438 | 1.006 | 2286.520 | 2304.030 | 0.992 | 3063.081 | 3077.578 | 0.995 |
| q36 | 147.465 | 153.227 | 0.962 | 215.753 | 224.069 | 0.963 | 515.334 | 534.555 | 0.964 |
| q37 | 36.553 | 35.593 | 1.027 | 69.408 | 68.615 | 1.012 | 143.407 | 140.332 | 1.022 |
| q38 | 35.783 | 34.904 | 1.025 | 68.584 | 68.074 | 1.007 | 146.608 | 143.063 | 1.025 |
| q39 | 15.346 | 14.637 | 1.048 | 27.800 | 26.892 | 1.034 | 58.673 | 56.411 | 1.040 |
| q40 | 117.222 | 116.740 | 1.004 | 420.597 | 419.689 | 1.002 | 657.608 | 656.894 | 1.001 |
| q41 | 17.590 | 25.000 | 0.704 | 39.109 | 111.650 | 0.350 | 75.177 | 161.868 | 0.464 |
| q42 | 13.425 | 13.060 | 1.028 | 24.352 | 23.637 | 1.030 | 51.874 | 50.618 | 1.025 |
| q43 | 28.457 | 27.501 | 1.035 | 41.294 | 40.240 | 1.026 | 99.229 | 95.446 | 1.040 |
| total | 9814.3 | 9941.5 | 0.987 | 24845.4 | 25178.2 | 0.987 | 44626.6 | 45199.4 | 0.987 |
warm outside 1.05x (gap >= 0.1 ms): []
warm over 1.05x but not confirmed (gap < 0.1 ms or median-of-rounds ratio <= 1.05: noise): []
first run outside 1.05x (gap >= 0.1 ms): [('q16', 1.729)]
result text differs head vs baseline: stable-head diffs []; head unstable against itself (tie order) []


## P2 range reads (10M store, release build, commit a184863c, base 80a28e49)

Script: `bench/compress_estimate/p2_bench.sh` (prep, splayed, parted stages)
and `p2_summary.py` (tables and the gate lines, printed from p2.tsv).
Loads: splayed copies g0 / g2 written by `.db.splayed.set` from
/skull/clickbench/rfsplayed; parted conversions p0 / p2 of
/skull/clickbench/hits_h.csv (`.csv.parted`, 1M rows per partition); a ragged
copy of p0 (last partition 9 keeps only its first 40 columns: no AdvEngineID
and none of the columns after it; ResolutionWidth, index 20, stays, because
the parted loader takes a shorter partition only as a missing column tail).
One load kind on disk at a time under /home/hetoku/data/tmp/p2_bench.
Per query: 5 alternated rounds, each a fresh process running it 6 times:
first run = run 1, 3-run total = runs 1-3, warm = best round's median of
runs 2-6.  Cold: 3 trials, page cache evicted per file (POSIX_FADV_DONTNEED;
the .sym is evicted with the store).  Answers: each query's full result
(`.csv.write` for tables, the printed value otherwise) compared byte for byte;
queries without asc:/desc: compared with lines sorted; variant b run twice.
On disk: g0 5 948 677 939 B, g2 3 307 128 731 B (1.80x); p0 4 807 844 987 B,
p2 2 072 366 907 B (2.32x).  Peak RSS over all 43 queries: g0 5 109 200 kB,
g2 8 101 948 kB, p0 14 864 816 kB, p2 15 743 720 kB.
Answers: identical on every arm (0 mismatches, 0 order differences).

### arm A: splayed g0 vs g2 (ms; ratio = g2/g0)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 0.995 | 0.01 | 0.01 | 0.991 | 0.01 | 0.01 | 0.990 | 0.01 | 0.01 | 1.018 | [] | same |
| q2 | 0.65 | 3.43 | 5.288 | 1.61 | 7.62 | 4.734 | 3.12 | 14.65 | 4.699 | 38.21 | 7.93 | 0.208 | [] | same |
| q3 | 3.79 | 0.00 | 0.001 | 5.52 | 0.04 | 0.006 | 13.17 | 0.04 | 0.003 | 94.54 | 0.04 | 0.000 | [] | same |
| q4 | 0.00 | 0.00 | 1.070 | 0.02 | 0.03 | 1.211 | 0.03 | 0.03 | 1.200 | 0.03 | 0.03 | 1.220 | [] | same |
| q5 | 0.00 | 0.00 | 1.259 | 0.03 | 0.03 | 0.897 | 0.04 | 0.03 | 0.915 | 0.03 | 0.03 | 0.994 | [] | same |
| q6 | 0.00 | 0.00 | 1.409 | 0.02 | 0.03 | 1.484 | 0.02 | 0.04 | 1.464 | 0.03 | 0.03 | 1.016 | [] | same |
| q7 | 0.00 | 0.00 | 1.119 | 0.02 | 0.03 | 1.660 | 0.02 | 0.04 | 1.571 | 0.02 | 0.04 | 1.770 | [] | same |
| q8 | 3.54 | 12.69 | 3.585 | 8.81 | 17.43 | 1.979 | 15.89 | 43.04 | 2.709 | 41.39 | 19.68 | 0.476 | [] | same |
| q9 | 75.13 | 75.28 | 1.002 | 112.55 | 131.21 | 1.166 | 265.23 | 287.73 | 1.085 | 665.44 | 156.55 | 0.235 | [RegionID UserID] | same |
| q10 | 68.60 | 77.23 | 1.126 | 97.17 | 136.21 | 1.402 | 237.51 | 297.81 | 1.254 | 403.47 | 164.59 | 0.408 | [RegionID UserID] | same |
| q11 | 16.31 | 16.45 | 1.009 | 25.01 | 50.49 | 2.019 | 57.76 | 83.48 | 1.445 | 246.75 | 67.57 | 0.274 | [UserID MobilePhoneModel] | same |
| q12 | 16.10 | 16.28 | 1.012 | 25.28 | 56.43 | 2.232 | 58.59 | 89.72 | 1.531 | 279.37 | 74.54 | 0.267 | [UserID MobilePhone MobilePhoneModel] | same |
| q13 | 32.34 | 32.22 | 0.996 | 41.48 | 55.37 | 1.335 | 106.90 | 120.18 | 1.124 | 113.55 | 64.37 | 0.567 | [SearchPhrase] | same |
| q14 | 63.27 | 63.16 | 0.998 | 88.81 | 117.31 | 1.321 | 219.49 | 247.11 | 1.126 | 304.71 | 141.66 | 0.465 | [UserID SearchPhrase] | same |
| q15 | 36.45 | 51.44 | 1.411 | 47.81 | 77.93 | 1.630 | 122.83 | 182.74 | 1.488 | 155.68 | 95.09 | 0.611 | [SearchPhrase] | same |
| q16 | 72.82 | 69.12 | 0.949 | 101.81 | 90.65 | 0.890 | 250.86 | 230.98 | 0.921 | 245.42 | 167.49 | 0.682 | [] | same |
| q17 | 95.90 | 123.10 | 1.284 | 133.37 | 166.22 | 1.246 | 329.94 | 413.65 | 1.254 | 458.42 | 294.68 | 0.643 | [] | same |
| q18 | 6.82 | 6.86 | 1.006 | 13.51 | 36.86 | 2.729 | 27.42 | 50.92 | 1.857 | 624.91 | 61.98 | 0.099 | [UserID SearchPhrase] | same |
| q19 | 141.17 | 166.63 | 1.180 | 212.97 | 255.88 | 1.201 | 501.27 | 596.17 | 1.189 | 674.52 | 479.71 | 0.711 | [EventTime] | same |
| q20 | 1.10 | 3.17 | 2.888 | 6.82 | 8.17 | 1.197 | 9.44 | 14.61 | 1.547 | 55.49 | 21.73 | 0.392 | [] | same |
| q21 | 42.22 | 41.99 | 0.995 | 45.28 | 51.21 | 1.131 | 132.45 | 136.48 | 1.030 | 77.39 | 92.90 | 1.200 | [URL] | same |
| q22 | 27.74 | 27.55 | 0.993 | 32.59 | 65.85 | 2.020 | 89.67 | 121.78 | 1.358 | 184.00 | 134.69 | 0.732 | [UserID URL SearchPhrase] | same |
| q23 | 48.86 | 49.01 | 1.003 | 60.44 | 98.66 | 1.633 | 159.04 | 197.23 | 1.240 | 413.08 | 206.37 | 0.500 | [Title UserID URL SearchPhrase] | same |
| q24 | 29.94 | 29.88 | 0.998 | 35.16 | 827.95 | 23.546 | 94.97 | 891.25 | 9.385 | 611.94 | 1463.61 | 2.392 | <100 columns> | same |
| q25 | 1.08 | 1.11 | 1.025 | 4.44 | 791.48 | 178.063 | 6.68 | 793.85 | 118.835 | 297.25 | 1446.28 | 4.866 | <100 columns> | same |
| q26 | 11.07 | 11.06 | 0.999 | 15.61 | 796.79 | 51.058 | 37.97 | 819.43 | 21.580 | 558.42 | 1455.72 | 2.607 | <100 columns> | same |
| q27 | 0.97 | 1.10 | 1.132 | 4.49 | 783.02 | 174.428 | 6.66 | 785.39 | 117.959 | 292.77 | 1443.96 | 4.932 | <100 columns> | same |
| q28 | 86.33 | 72.33 | 0.838 | 88.94 | 83.97 | 0.944 | 265.23 | 228.97 | 0.863 | 224.11 | 127.58 | 0.569 | [URL] | same |
| q29 | 350.58 | 350.90 | 1.001 | 440.03 | 443.62 | 1.008 | 1143.04 | 1153.56 | 1.009 | 510.18 | 491.90 | 0.964 | [Referer] | same |
| q30 | 0.02 | 0.02 | 0.992 | 2.77 | 9.49 | 3.419 | 2.84 | 9.53 | 3.355 | 53.19 | 12.57 | 0.236 | [ResolutionWidth] | same |
| q31 | 51.74 | 52.08 | 1.006 | 70.40 | 114.33 | 1.624 | 175.47 | 221.74 | 1.264 | 317.26 | 142.26 | 0.448 | [ClientIP IsRefresh ResolutionWidth SearchEngineID SearchPhrase] | same |
| q32 | 59.84 | 59.65 | 0.997 | 86.55 | 122.84 | 1.419 | 207.15 | 243.92 | 1.177 | 443.89 | 285.56 | 0.643 | [ClientIP IsRefresh ResolutionWidth SearchPhrase] | same |
| q33 | 207.66 | 231.81 | 1.116 | 371.55 | 394.89 | 1.063 | 799.40 | 867.71 | 1.085 | 598.50 | 593.67 | 0.992 | [] | same |
| q34 | 39.25 | 38.63 | 0.984 | 70.30 | 72.01 | 1.024 | 150.40 | 149.67 | 0.995 | 138.37 | 110.57 | 0.799 | [] | same |
| q35 | 39.25 | 38.75 | 0.987 | 69.84 | 71.72 | 1.027 | 150.67 | 149.17 | 0.990 | 138.13 | 109.70 | 0.794 | [] | same |
| q36 | 66.56 | 68.04 | 1.022 | 90.61 | 87.89 | 0.970 | 224.81 | 227.58 | 1.012 | 163.63 | 132.62 | 0.811 | [] | same |
| q37 | 16.61 | 16.58 | 0.998 | 27.20 | 47.29 | 1.739 | 60.68 | 80.94 | 1.334 | 41.97 | 92.43 | 2.202 | [EventDate URL IsRefresh DontCountHits] | same |
| q38 | 15.04 | 15.01 | 0.998 | 24.41 | 45.25 | 1.854 | 55.53 | 75.90 | 1.367 | 38.88 | 85.59 | 2.201 | [Title EventDate IsRefresh DontCountHits] | same |
| q39 | 7.10 | 7.19 | 1.012 | 14.61 | 38.87 | 2.661 | 30.02 | 53.81 | 1.792 | 42.55 | 88.72 | 2.085 | [EventDate URL IsRefresh IsLink IsDownload] | same |
| q40 | 59.01 | 58.54 | 0.992 | 90.51 | 132.40 | 1.463 | 212.86 | 252.34 | 1.185 | 117.68 | 236.73 | 2.012 | [EventDate CounterID URL Referer IsRefresh TraficSourceID SearchEngineID AdvEngineID] | same |
| q41 | 5.78 | 5.60 | 0.969 | 13.09 | 29.94 | 2.287 | 25.07 | 41.51 | 1.656 | 74.18 | 51.80 | 0.698 | [EventDate CounterID IsRefresh TraficSourceID] | same |
| q42 | 4.59 | 11.98 | 2.611 | 11.40 | 37.77 | 3.312 | 20.65 | 62.44 | 3.024 | 37.37 | 49.78 | 1.332 | [EventDate CounterID IsRefresh DontCountHits] | same |
| q43 | 9.02 | 9.02 | 0.999 | 18.35 | 48.99 | 2.670 | 37.45 | 67.55 | 1.804 | 39.91 | 151.26 | 3.790 | [EventTime EventDate CounterID IsRefresh DontCountHits] | same |

```
totals over all 43: warm 1814.3 vs 1914.9 (1.055); first run 2611.2 vs 6404.2 (2.453); 3-run 6308.3 vs 10304.7 (1.634)
answer_mismatches none
order_differences none
scan_only n=13 warm_gmean=0.730 warm_worst=5.288 first_gmean=1.007 first_worst=4.734 cold_gmean=0.394 cold_worst=2.085
scan_only_materializing q18(P3-deferred) q39(P3-deferred)
worst5 warm [(5.288, 'q2'), (3.585, 'q8'), (1.284, 'q17'), (1.119, 'q7'), (1.116, 'q33')]
worst5 cold [(2.085, 'q39'), (1.77, 'q7'), (1.22, 'q4'), (1.018, 'q1'), (0.992, 'q33')]
worst5 first [(4.734, 'q2'), (2.729, 'q18'), (2.661, 'q39'), (1.979, 'q8'), (1.66, 'q7')]
worst5 materializing_warm [(2.888, 'q20'), (2.611, 'q42'), (1.411, 'q15'), (1.409, 'q6'), (1.259, 'q5')]
first_run_total_ratio 2.453 (PI after P1: 2.99)
pi_row q41 warm 0.969 first 2.287 3run 1.656
pi_row q42 warm 2.611 first 3.312 3run 3.024
```

### arm B: parted p0 vs p2 (ms; ratio = p2/p0)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 0.952 | 0.01 | 0.01 | 0.606 | 0.01 | 0.01 | 0.628 | 0.01 | 0.01 | 0.590 | [] | same |
| q2 | 6.58 | 9.83 | 1.494 | 17.03 | 18.16 | 1.066 | 30.48 | 37.95 | 1.245 | 47.81 | 17.94 | 0.375 | [] | same |
| q3 | 540.30 | 12.90 | 0.024 | 546.05 | 17.68 | 0.032 | 1632.34 | 43.60 | 0.027 | 564.14 | 19.39 | 0.034 | [] | same |
| q4 | 273.97 | 9.98 | 0.036 | 320.94 | 14.93 | 0.047 | 875.38 | 35.56 | 0.041 | 364.83 | 33.03 | 0.091 | [] | same |
| q5 | 274.29 | 0.00 | 0.000 | 323.55 | 0.03 | 0.000 | 878.66 | 0.03 | 0.000 | 365.09 | 0.04 | 0.000 | [] | same |
| q6 | 341.74 | 343.32 | 1.005 | 9648.46 | 9631.52 | 0.998 | 10333.93 | 10325.20 | 0.999 | 9688.53 | 9730.10 | 1.004 | [SearchPhrase] | same |
| q7 | 524.77 | 4.97 | 0.009 | 553.22 | 9.25 | 0.017 | 1610.32 | 19.56 | 0.012 | 574.75 | 9.61 | 0.017 | [] | same |
| q8 | 8.64 | 26.08 | 3.020 | 10.12 | 32.62 | 3.223 | 27.66 | 85.45 | 3.089 | 43.77 | 32.65 | 0.746 | [] | same |
| q9 | 85.40 | 83.24 | 0.975 | 151.93 | 174.92 | 1.151 | 324.13 | 346.82 | 1.070 | 390.50 | 194.38 | 0.498 | [RegionID UserID] | same |
| q10 | 78.88 | 85.98 | 1.090 | 157.68 | 184.35 | 1.169 | 316.28 | 362.88 | 1.147 | 447.95 | 208.92 | 0.466 | [RegionID UserID] | same |
| q11 | 357.41 | 348.47 | 0.975 | 576.50 | 1285.17 | 2.229 | 1290.19 | 1991.25 | 1.543 | 10386.90 | 2549.53 | 0.245 | <105 columns> | same |
| q12 | 353.55 | 348.32 | 0.985 | 571.92 | 1324.12 | 2.315 | 1286.72 | 2077.45 | 1.615 | 10332.90 | 2461.06 | 0.238 | <105 columns> | same |
| q13 | 40.30 | 40.08 | 0.994 | 70.48 | 82.55 | 1.171 | 151.85 | 168.81 | 1.112 | 154.95 | 93.44 | 0.603 | [SearchPhrase] | same |
| q14 | 1116.70 | 1118.73 | 1.002 | 1395.79 | 2425.98 | 1.738 | 3646.17 | 4776.23 | 1.310 | 9982.71 | 4084.80 | 0.409 | <105 columns> | same |
| q15 | 46.75 | 64.81 | 1.386 | 89.81 | 119.80 | 1.334 | 185.36 | 251.78 | 1.358 | 218.12 | 133.39 | 0.612 | [SearchPhrase] | same |
| q16 | 72.34 | 69.73 | 0.964 | 98.60 | 91.66 | 0.930 | 243.76 | 233.18 | 0.957 | 245.47 | 163.00 | 0.664 | [] | same |
| q17 | 95.53 | 123.20 | 1.290 | 133.67 | 168.75 | 1.262 | 326.62 | 416.80 | 1.276 | 458.69 | 289.93 | 0.632 | [] | same |
| q18 | 17.02 | 96.59 | 5.676 | 82.89 | 154.56 | 1.865 | 117.42 | 352.66 | 3.003 | 338.05 | 177.74 | 0.526 | [] | same |
| q19 | 141.05 | 231.04 | 1.638 | 335.78 | 417.38 | 1.243 | 621.69 | 879.86 | 1.415 | 758.07 | 555.79 | 0.733 | [EventTime] | same |
| q20 | 4.65 | 4.23 | 0.910 | 14.61 | 13.44 | 0.920 | 24.28 | 22.08 | 0.909 | 157.50 | 26.84 | 0.170 | [] | same |
| q21 | 41.94 | 41.78 | 0.996 | 46.90 | 56.37 | 1.202 | 131.78 | 140.89 | 1.069 | 89.56 | 100.97 | 1.128 | [URL] | same |
| q22 | 60.81 | 60.55 | 0.996 | 86.26 | 117.42 | 1.361 | 209.81 | 245.06 | 1.168 | 234.23 | 188.82 | 0.806 | [UserID URL SearchPhrase] | same |
| q23 | 93.79 | 93.43 | 0.996 | 207.73 | 975.62 | 4.696 | 396.18 | 1169.84 | 2.953 | 10458.40 | 2144.38 | 0.205 | <105 columns> | same |
| q24 | 44.83 | 43.79 | 0.977 | 129.40 | 914.32 | 7.066 | 222.10 | 1006.23 | 4.531 | 9603.40 | 1992.05 | 0.207 | <105 columns> | same |
| q25 | 1056.03 | 1052.35 | 0.997 | 1310.78 | 2493.40 | 1.902 | 3428.63 | 4669.08 | 1.362 | 9853.74 | 3924.10 | 0.398 | <105 columns> | same |
| q26 | 1113.89 | 1104.44 | 0.992 | 1459.83 | 2632.38 | 1.803 | 3690.92 | 4967.34 | 1.346 | 10048.50 | 4063.58 | 0.404 | <105 columns> | same |
| q27 | 1067.52 | 1064.85 | 0.997 | 1321.60 | 2538.17 | 1.921 | 3465.72 | 4763.56 | 1.374 | 9931.12 | 4025.96 | 0.405 | <105 columns> | same |
| q28 | 107.17 | 105.88 | 0.988 | 166.71 | 168.28 | 1.009 | 386.06 | 384.41 | 0.996 | 325.30 | 213.97 | 0.658 | [URL] | same |
| q29 | 356.74 | 356.98 | 1.001 | 467.90 | 478.22 | 1.022 | 1188.20 | 1196.51 | 1.007 | 551.13 | 518.06 | 0.940 | [Referer] | same |
| q30 | 267.51 | 266.83 | 0.997 | 276.80 | 287.61 | 1.039 | 830.20 | 829.11 | 0.999 | 285.37 | 287.75 | 1.008 | [ResolutionWidth] | same |
| q31 | 55.71 | 146.35 | 2.627 | 125.14 | 222.52 | 1.778 | 237.99 | 517.07 | 2.173 | 434.18 | 253.06 | 0.583 | [SearchPhrase] | same |
| q32 | 69.11 | 171.18 | 2.477 | 179.51 | 291.09 | 1.622 | 318.35 | 633.76 | 1.991 | 624.33 | 463.01 | 0.742 | [SearchPhrase] | same |
| q33 | 212.19 | 242.79 | 1.144 | 371.49 | 413.57 | 1.113 | 806.74 | 909.70 | 1.128 | 614.75 | 753.17 | 1.225 | [] | same |
| q34 | 43.19 | 46.34 | 1.073 | 95.59 | 97.05 | 1.015 | 183.40 | 191.10 | 1.042 | 180.01 | 145.04 | 0.806 | [] | same |
| q35 | 43.06 | 46.47 | 1.079 | 95.33 | 98.16 | 1.030 | 182.69 | 192.45 | 1.053 | 180.18 | 146.00 | 0.810 | [] | same |
| q36 | 144.81 | 181.98 | 1.257 | 214.66 | 252.28 | 1.175 | 513.86 | 623.38 | 1.213 | 300.37 | 269.32 | 0.897 | [] | same |
| q37 | 35.92 | 34.02 | 0.947 | 69.23 | 87.35 | 1.262 | 141.65 | 156.47 | 1.105 | 227.11 | 131.86 | 0.581 | [EventDate URL IsRefresh DontCountHits] | same |
| q38 | 34.90 | 32.80 | 0.940 | 67.34 | 89.53 | 1.329 | 142.75 | 154.88 | 1.085 | 230.93 | 130.62 | 0.566 | [Title EventDate IsRefresh DontCountHits] | same |
| q39 | 15.09 | 13.34 | 0.884 | 27.29 | 44.80 | 1.642 | 57.63 | 76.88 | 1.334 | 115.60 | 54.11 | 0.468 | [EventDate URL IsRefresh IsLink IsDownload] | same |
| q40 | 116.43 | 156.03 | 1.340 | 420.06 | 461.43 | 1.098 | 656.09 | 781.42 | 1.191 | 786.82 | 577.51 | 0.734 | [EventDate IsRefresh] | same |
| q41 | 17.58 | 22.25 | 1.266 | 39.07 | 63.86 | 1.635 | 75.49 | 112.03 | 1.484 | 268.00 | 227.34 | 0.848 | [EventDate CounterID IsRefresh TraficSourceID URLHash] | same |
| q42 | 13.15 | 22.57 | 1.716 | 24.11 | 57.65 | 2.391 | 50.87 | 105.34 | 2.071 | 193.49 | 173.00 | 0.894 | [EventDate CounterID IsRefresh WindowClientWidth WindowClientHeight DontCountHits] | same |
| q43 | 28.05 | 26.16 | 0.933 | 40.61 | 64.35 | 1.585 | 97.04 | 118.75 | 1.224 | 130.26 | 162.87 | 1.250 | [EventTime EventDate IsRefresh DontCountHits] | same |

```
totals over all 43: warm 9419.3 vs 8354.7 (0.887); first run 22342.4 vs 29072.3 (1.301); 3-run 41337.4 vs 46372.4 (1.122)
answer_mismatches none
order_differences none
scan_only n=13 warm_gmean=0.535 warm_worst=5.676 first_gmean=0.523 first_worst=3.223 cold_gmean=0.336 cold_worst=1.225
scan_only_materializing q39(P3-deferred)
worst5 warm [(5.676, 'q18'), (3.02, 'q8'), (1.494, 'q2'), (1.29, 'q17'), (1.144, 'q33')]
worst5 cold [(1.225, 'q33'), (0.81, 'q35'), (0.806, 'q34'), (0.746, 'q8'), (0.664, 'q16')]
worst5 first [(3.223, 'q8'), (1.865, 'q18'), (1.642, 'q39'), (1.262, 'q17'), (1.113, 'q33')]
worst5 materializing_warm [(2.627, 'q31'), (2.477, 'q32'), (1.716, 'q42'), (1.638, 'q19'), (1.386, 'q15')]
pi_row q41 warm 1.266 first 1.635 3run 1.484
pi_row q42 warm 1.716 first 2.391 3run 2.071
```

### arm C0: splayed g0, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 0.916 | 0.01 | 0.01 | 0.999 | 0.01 | 0.01 | 0.997 | nan | nan | nan |  | same |
| q2 | 1.11 | 0.64 | 0.576 | 2.03 | 1.61 | 0.793 | 4.33 | 3.10 | 0.716 | nan | nan | nan |  | same |
| q3 | 3.78 | 3.79 | 1.001 | 5.65 | 5.75 | 1.019 | 13.27 | 13.42 | 1.011 | nan | nan | nan |  | same |
| q4 | 0.00 | 0.00 | 1.046 | 0.02 | 0.03 | 1.189 | 0.02 | 0.03 | 1.184 | nan | nan | nan |  | same |
| q5 | 0.00 | 0.00 | 1.010 | 0.03 | 0.02 | 0.884 | 0.03 | 0.02 | 0.902 | nan | nan | nan |  | same |
| q6 | 0.00 | 0.00 | 1.070 | 0.03 | 0.02 | 0.824 | 0.03 | 0.02 | 0.831 | nan | nan | nan |  | same |
| q7 | 0.00 | 0.00 | 1.036 | 0.02 | 0.02 | 1.033 | 0.02 | 0.02 | 1.046 | nan | nan | nan |  | same |
| q8 | 3.49 | 3.56 | 1.021 | 8.86 | 8.94 | 1.009 | 16.05 | 16.18 | 1.008 | nan | nan | nan |  | same |
| q9 | 75.90 | 75.33 | 0.992 | 115.44 | 113.47 | 0.983 | 267.67 | 264.46 | 0.988 | nan | nan | nan |  | same |
| q10 | 69.23 | 69.67 | 1.006 | 97.35 | 104.22 | 1.071 | 239.68 | 245.66 | 1.025 | nan | nan | nan |  | same |
| q11 | 16.20 | 16.26 | 1.003 | 24.96 | 25.29 | 1.013 | 57.96 | 58.09 | 1.002 | nan | nan | nan |  | same |
| q12 | 16.40 | 16.10 | 0.982 | 25.93 | 25.66 | 0.989 | 59.87 | 58.54 | 0.978 | nan | nan | nan |  | same |
| q13 | 31.90 | 32.30 | 1.013 | 41.01 | 41.32 | 1.008 | 106.01 | 105.76 | 0.998 | nan | nan | nan |  | same |
| q14 | 62.72 | 63.05 | 1.005 | 90.23 | 90.11 | 0.999 | 224.77 | 220.95 | 0.983 | nan | nan | nan |  | same |
| q15 | 36.37 | 36.63 | 1.007 | 48.16 | 48.24 | 1.002 | 123.53 | 125.32 | 1.015 | nan | nan | nan |  | same |
| q16 | 69.87 | 72.59 | 1.039 | 94.14 | 97.87 | 1.040 | 234.21 | 245.34 | 1.048 | nan | nan | nan |  | same |
| q17 | 93.27 | 95.00 | 1.019 | 128.92 | 134.41 | 1.043 | 320.07 | 327.26 | 1.022 | nan | nan | nan |  | same |
| q18 | 6.90 | 6.88 | 0.997 | 13.92 | 13.82 | 0.993 | 27.97 | 27.68 | 0.989 | nan | nan | nan |  | same |
| q19 | 139.60 | 141.25 | 1.012 | 208.89 | 210.19 | 1.006 | 488.93 | 499.67 | 1.022 | nan | nan | nan |  | same |
| q20 | 1.12 | 1.15 | 1.023 | 6.89 | 6.84 | 0.993 | 9.42 | 9.40 | 0.997 | nan | nan | nan |  | same |
| q21 | 41.92 | 42.34 | 1.010 | 45.19 | 46.08 | 1.020 | 129.17 | 132.85 | 1.028 | nan | nan | nan |  | same |
| q22 | 27.61 | 27.72 | 1.004 | 31.83 | 32.36 | 1.017 | 87.77 | 89.04 | 1.015 | nan | nan | nan |  | same |
| q23 | 48.75 | 48.76 | 1.000 | 60.27 | 60.32 | 1.001 | 158.26 | 158.27 | 1.000 | nan | nan | nan |  | same |
| q24 | 29.28 | 29.90 | 1.021 | 34.23 | 35.97 | 1.051 | 93.17 | 97.15 | 1.043 | nan | nan | nan |  | same |
| q25 | 0.95 | 1.03 | 1.078 | 4.32 | 4.39 | 1.016 | 6.38 | 6.65 | 1.041 | nan | nan | nan |  | same |
| q26 | 10.74 | 11.08 | 1.032 | 15.21 | 15.42 | 1.014 | 36.81 | 37.63 | 1.022 | nan | nan | nan |  | same |
| q27 | 0.93 | 1.08 | 1.170 | 4.34 | 4.44 | 1.023 | 6.33 | 6.63 | 1.048 | nan | nan | nan |  | same |
| q28 | 82.56 | 86.36 | 1.046 | 85.19 | 89.28 | 1.048 | 251.59 | 263.23 | 1.046 | nan | nan | nan |  | same |
| q29 | 353.78 | 350.44 | 0.991 | 444.64 | 440.71 | 0.991 | 1159.04 | 1147.23 | 0.990 | nan | nan | nan |  | same |
| q30 | 0.03 | 0.02 | 0.588 | 2.94 | 2.91 | 0.990 | 3.01 | 2.98 | 0.990 | nan | nan | nan |  | same |
| q31 | 52.98 | 51.90 | 0.980 | 71.08 | 70.10 | 0.986 | 178.14 | 174.78 | 0.981 | nan | nan | nan |  | same |
| q32 | 59.52 | 60.00 | 1.008 | 85.27 | 88.10 | 1.033 | 205.17 | 207.93 | 1.013 | nan | nan | nan |  | same |
| q33 | 202.05 | 208.10 | 1.030 | 362.74 | 373.46 | 1.030 | 775.96 | 798.94 | 1.030 | nan | nan | nan |  | same |
| q34 | 39.47 | 39.18 | 0.993 | 70.30 | 71.42 | 1.016 | 150.12 | 150.65 | 1.004 | nan | nan | nan |  | same |
| q35 | 39.09 | 39.03 | 0.998 | 70.14 | 69.35 | 0.989 | 148.37 | 148.54 | 1.001 | nan | nan | nan |  | same |
| q36 | 64.05 | 66.85 | 1.044 | 89.41 | 90.80 | 1.016 | 218.76 | 227.06 | 1.038 | nan | nan | nan |  | same |
| q37 | 17.19 | 16.59 | 0.965 | 27.80 | 27.04 | 0.973 | 62.33 | 60.18 | 0.965 | nan | nan | nan |  | same |
| q38 | 15.83 | 15.02 | 0.949 | 25.25 | 24.27 | 0.961 | 57.70 | 55.04 | 0.954 | nan | nan | nan |  | same |
| q39 | 7.48 | 7.10 | 0.949 | 14.77 | 14.33 | 0.970 | 29.91 | 29.13 | 0.974 | nan | nan | nan |  | same |
| q40 | 59.09 | 58.96 | 0.998 | 89.70 | 90.02 | 1.004 | 209.60 | 209.05 | 0.997 | nan | nan | nan |  | same |
| q41 | 5.86 | 5.62 | 0.960 | 13.34 | 13.24 | 0.992 | 25.16 | 25.24 | 1.003 | nan | nan | nan |  | same |
| q42 | 4.71 | 4.46 | 0.947 | 11.25 | 11.00 | 0.978 | 20.96 | 20.42 | 0.974 | nan | nan | nan |  | same |
| q43 | 9.30 | 8.99 | 0.967 | 18.86 | 18.72 | 0.992 | 38.44 | 37.47 | 0.975 | nan | nan | nan |  | same |

```
totals over all 43: warm 1801.0 vs 1814.7 (1.008); first run 2590.5 vs 2621.6 (1.012); 3-run 6246.0 vs 6307.0 (1.010)
answer_mismatches none
order_differences none
gen0 warm_gmean=0.982 warm_worst=1.170 worst5 [(1.17, 'q27'), (1.078, 'q25'), (1.07, 'q6'), (1.046, 'q4'), (1.046, 'q28')]
```

### arm C1: parted p0, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.069 | 0.01 | 0.01 | 0.961 | 0.01 | 0.01 | 0.961 | nan | nan | nan |  | same |
| q2 | 6.60 | 6.59 | 0.998 | 16.99 | 17.02 | 1.002 | 30.64 | 30.64 | 1.000 | nan | nan | nan |  | same |
| q3 | 535.48 | 535.69 | 1.000 | 551.02 | 547.71 | 0.994 | 1633.60 | 1634.45 | 1.001 | nan | nan | nan |  | same |
| q4 | 276.73 | 274.97 | 0.994 | 328.86 | 319.19 | 0.971 | 895.80 | 876.34 | 0.978 | nan | nan | nan |  | same |
| q5 | 276.18 | 275.29 | 0.997 | 324.54 | 323.64 | 0.997 | 880.88 | 880.97 | 1.000 | nan | nan | nan |  | same |
| q6 | 344.26 | 342.00 | 0.993 | 9689.67 | 9684.79 | 0.999 | 10384.85 | 10371.18 | 0.999 | nan | nan | nan |  | same |
| q7 | 525.31 | 526.47 | 1.002 | 558.69 | 561.35 | 1.005 | 1616.75 | 1614.78 | 0.999 | nan | nan | nan |  | same |
| q8 | 9.10 | 8.55 | 0.940 | 21.03 | 10.20 | 0.485 | 39.58 | 27.68 | 0.699 | nan | nan | nan |  | same |
| q9 | 89.31 | 85.81 | 0.961 | 176.81 | 153.50 | 0.868 | 357.14 | 328.43 | 0.920 | nan | nan | nan |  | same |
| q10 | 87.46 | 79.54 | 0.909 | 206.09 | 154.12 | 0.748 | 384.36 | 313.15 | 0.815 | nan | nan | nan |  | same |
| q11 | 358.17 | 355.63 | 0.993 | 584.11 | 581.16 | 0.995 | 1304.84 | 1296.63 | 0.994 | nan | nan | nan |  | same |
| q12 | 359.16 | 354.16 | 0.986 | 583.13 | 581.63 | 0.997 | 1303.94 | 1296.90 | 0.995 | nan | nan | nan |  | same |
| q13 | 39.96 | 40.09 | 1.003 | 71.04 | 71.14 | 1.001 | 151.23 | 152.05 | 1.005 | nan | nan | nan |  | same |
| q14 | 1128.22 | 1116.40 | 0.990 | 1422.55 | 1406.34 | 0.989 | 3701.36 | 3649.07 | 0.986 | nan | nan | nan |  | same |
| q15 | 46.82 | 46.51 | 0.993 | 90.37 | 90.32 | 0.999 | 188.51 | 187.64 | 0.995 | nan | nan | nan |  | same |
| q16 | 76.23 | 72.43 | 0.950 | 144.47 | 100.68 | 0.697 | 299.87 | 247.16 | 0.824 | nan | nan | nan |  | same |
| q17 | 103.50 | 95.71 | 0.925 | 203.57 | 135.25 | 0.664 | 414.76 | 330.23 | 0.796 | nan | nan | nan |  | same |
| q18 | 17.13 | 17.02 | 0.994 | 83.87 | 83.58 | 0.997 | 118.60 | 120.49 | 1.016 | nan | nan | nan |  | same |
| q19 | 140.84 | 141.68 | 1.006 | 334.35 | 338.78 | 1.013 | 621.70 | 624.22 | 1.004 | nan | nan | nan |  | same |
| q20 | 4.63 | 4.66 | 1.006 | 14.37 | 14.44 | 1.005 | 23.92 | 24.03 | 1.005 | nan | nan | nan |  | same |
| q21 | 41.65 | 42.21 | 1.014 | 46.59 | 47.57 | 1.021 | 130.42 | 133.49 | 1.024 | nan | nan | nan |  | same |
| q22 | 61.05 | 61.04 | 1.000 | 86.10 | 86.61 | 1.006 | 208.14 | 209.74 | 1.008 | nan | nan | nan |  | same |
| q23 | 93.81 | 94.61 | 1.008 | 211.08 | 213.47 | 1.011 | 398.94 | 404.97 | 1.015 | nan | nan | nan |  | same |
| q24 | 44.61 | 44.82 | 1.005 | 131.29 | 132.09 | 1.006 | 220.76 | 222.73 | 1.009 | nan | nan | nan |  | same |
| q25 | 1073.31 | 1057.68 | 0.985 | 1333.96 | 1324.88 | 0.993 | 3491.04 | 3457.73 | 0.990 | nan | nan | nan |  | same |
| q26 | 1126.64 | 1108.97 | 0.984 | 1479.28 | 1473.01 | 0.996 | 3739.92 | 3699.15 | 0.989 | nan | nan | nan |  | same |
| q27 | 1081.98 | 1069.87 | 0.989 | 1342.24 | 1339.71 | 0.998 | 3515.27 | 3482.80 | 0.991 | nan | nan | nan |  | same |
| q28 | 91.77 | 107.21 | 1.168 | 147.33 | 165.57 | 1.124 | 332.84 | 382.56 | 1.149 | nan | nan | nan |  | same |
| q29 | 363.62 | 358.22 | 0.985 | 475.57 | 472.47 | 0.993 | 1205.25 | 1190.23 | 0.988 | nan | nan | nan |  | same |
| q30 | 264.87 | 266.35 | 1.006 | 280.50 | 276.89 | 0.987 | 829.64 | 819.75 | 0.988 | nan | nan | nan |  | same |
| q31 | 55.20 | 55.76 | 1.010 | 124.29 | 124.88 | 1.005 | 237.29 | 238.11 | 1.003 | nan | nan | nan |  | same |
| q32 | 69.19 | 68.97 | 0.997 | 180.26 | 178.97 | 0.993 | 321.53 | 319.79 | 0.995 | nan | nan | nan |  | same |
| q33 | 216.09 | 212.58 | 0.984 | 461.65 | 377.16 | 0.817 | 901.81 | 808.26 | 0.896 | nan | nan | nan |  | same |
| q34 | 43.40 | 43.16 | 0.995 | 97.46 | 94.99 | 0.975 | 188.36 | 185.06 | 0.982 | nan | nan | nan |  | same |
| q35 | 387.32 | 43.21 | 0.112 | 2322.52 | 96.04 | 0.041 | 3107.97 | 183.01 | 0.059 | nan | nan | nan |  | same |
| q36 | 148.87 | 146.00 | 0.981 | 217.93 | 214.84 | 0.986 | 518.94 | 512.59 | 0.988 | nan | nan | nan |  | same |
| q37 | 36.47 | 35.86 | 0.983 | 69.81 | 69.09 | 0.990 | 144.11 | 141.47 | 0.982 | nan | nan | nan |  | same |
| q38 | 35.63 | 34.95 | 0.981 | 67.78 | 67.70 | 0.999 | 145.01 | 144.27 | 0.995 | nan | nan | nan |  | same |
| q39 | 15.48 | 15.09 | 0.975 | 27.62 | 27.18 | 0.984 | 59.56 | 57.98 | 0.974 | nan | nan | nan |  | same |
| q40 | 117.34 | 116.40 | 0.992 | 422.09 | 419.54 | 0.994 | 662.64 | 655.30 | 0.989 | nan | nan | nan |  | same |
| q41 | 17.64 | 17.53 | 0.994 | 39.03 | 39.12 | 1.002 | 74.52 | 74.83 | 1.004 | nan | nan | nan |  | same |
| q42 | 13.52 | 13.07 | 0.967 | 24.18 | 23.87 | 0.987 | 51.47 | 50.55 | 0.982 | nan | nan | nan |  | same |
| q43 | 28.39 | 27.96 | 0.985 | 40.79 | 40.65 | 0.997 | 98.25 | 97.15 | 0.989 | nan | nan | nan |  | same |

```
totals over all 43: warm 9853.0 vs 9420.7 (0.956); first run 25034.9 vs 22481.2 (0.898); 3-run 44936.0 vs 41477.6 (0.923)
answer_mismatches none
order_differences none
gen0 warm_gmean=0.943 warm_worst=1.168 worst5 [(1.168, 'q28'), (1.069, 'q1'), (1.014, 'q21'), (1.01, 'q31'), (1.008, 'q23')]
q16 first run base 144.5 ms, p2 100.7 ms (carry 2)
```

### arm R: parted ragged, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 0.963 | 0.01 | 0.01 | 0.977 | 0.01 | 0.01 | 0.978 | nan | nan | nan |  | same |
| q2 | 7.73 | 8.78 | 1.136 | 18.17 | 18.60 | 1.024 | 33.95 | 36.45 | 1.074 | nan | nan | nan |  | same |
| q3 | 578.33 | 274.39 | 0.474 | 592.51 | 286.43 | 0.483 | 1757.86 | 838.04 | 0.477 | nan | nan | nan |  | same |
| q4 | 279.41 | 275.59 | 0.986 | 327.15 | 320.92 | 0.981 | 888.31 | 875.27 | 0.985 | nan | nan | nan |  | same |
| q5 | 277.18 | 274.38 | 0.990 | 326.30 | 321.24 | 0.984 | 888.94 | 873.79 | 0.983 | nan | nan | nan |  | same |
| q6 | 344.61 | 345.84 | 1.004 | 9719.35 | 9746.01 | 1.003 | 10417.85 | 10446.22 | 1.003 | nan | nan | nan |  | same |
| q7 | 529.83 | 528.19 | 0.997 | 553.66 | 557.71 | 1.007 | 1612.25 | 1620.38 | 1.005 | nan | nan | nan |  | same |
| q8 | 9.62 | 14.12 | 1.468 | 22.56 | 16.59 | 0.735 | 42.02 | 45.32 | 1.078 | nan | nan | nan |  | same |
| q9 | 89.22 | 85.78 | 0.961 | 177.20 | 155.12 | 0.875 | 358.80 | 329.99 | 0.920 | nan | nan | nan |  | same |
| q10 | 87.75 | 82.70 | 0.942 | 203.49 | 159.60 | 0.784 | 388.26 | 327.99 | 0.845 | nan | nan | nan |  | same |
| q11 | 377.48 | 373.51 | 0.989 | 659.69 | 655.83 | 0.994 | 1418.56 | 1409.88 | 0.994 | nan | nan | nan |  | same |
| q12 | 374.41 | 370.60 | 0.990 | 656.45 | 651.22 | 0.992 | 1410.51 | 1398.69 | 0.992 | nan | nan | nan |  | same |
| q13 | 40.03 | 40.39 | 1.009 | 70.73 | 70.97 | 1.003 | 151.95 | 153.32 | 1.009 | nan | nan | nan |  | same |
| q14 | 1187.45 | 1170.07 | 0.985 | 1535.19 | 1502.05 | 0.978 | 3926.25 | 3871.90 | 0.986 | nan | nan | nan |  | same |
| q15 | 46.51 | 46.32 | 0.996 | 89.84 | 89.77 | 0.999 | 187.90 | 187.62 | 0.998 | nan | nan | nan |  | same |
| q16 | 76.12 | 72.54 | 0.953 | 147.95 | 99.33 | 0.671 | 302.53 | 245.21 | 0.811 | nan | nan | nan |  | same |
| q17 | 103.70 | 95.15 | 0.918 | 205.98 | 135.02 | 0.655 | 416.35 | 328.33 | 0.789 | nan | nan | nan |  | same |
| q18 | 17.09 | 16.99 | 0.994 | 86.74 | 84.14 | 0.970 | 121.09 | 118.07 | 0.975 | nan | nan | nan |  | same |
| q19 | 141.36 | 141.32 | 1.000 | 337.89 | 336.63 | 0.996 | 623.79 | 624.58 | 1.001 | nan | nan | nan |  | same |
| q20 | 4.47 | 4.52 | 1.011 | 14.21 | 14.31 | 1.007 | 23.47 | 23.75 | 1.012 | nan | nan | nan |  | same |
| q21 | 41.74 | 41.84 | 1.003 | 46.64 | 46.74 | 1.002 | 130.41 | 130.94 | 1.004 | nan | nan | nan |  | same |
| q22 | 60.64 | 60.60 | 0.999 | 86.23 | 87.11 | 1.010 | 209.90 | 210.55 | 1.003 | nan | nan | nan |  | same |
| q23 | 93.56 | 94.03 | 1.005 | 273.81 | 269.62 | 0.985 | 465.64 | 464.65 | 0.998 | nan | nan | nan |  | same |
| q24 | 44.49 | 44.77 | 1.006 | 193.57 | 192.45 | 0.994 | 287.89 | 286.46 | 0.995 | nan | nan | nan |  | same |
| q25 | 1122.11 | 1111.83 | 0.991 | 1439.10 | 1421.51 | 0.988 | 3707.26 | 3654.40 | 0.986 | nan | nan | nan |  | same |
| q26 | 1182.70 | 1167.93 | 0.988 | 1595.74 | 1577.64 | 0.989 | 3975.71 | 3915.87 | 0.985 | nan | nan | nan |  | same |
| q27 | 1137.22 | 1125.48 | 0.990 | 1451.73 | 1429.17 | 0.984 | 3740.65 | 3689.33 | 0.986 | nan | nan | nan |  | same |
| q28 | 91.88 | 107.85 | 1.174 | 147.68 | 165.83 | 1.123 | 334.28 | 383.42 | 1.147 | nan | nan | nan |  | same |
| q29 | 362.83 | 359.28 | 0.990 | 475.31 | 469.26 | 0.987 | 1205.11 | 1192.57 | 0.990 | nan | nan | nan |  | same |
| q30 | 265.22 | 269.17 | 1.015 | 282.56 | 277.77 | 0.983 | 833.58 | 830.24 | 0.996 | nan | nan | nan |  | same |
| q31 | 55.26 | 55.39 | 1.002 | 125.28 | 124.48 | 0.994 | 237.67 | 238.05 | 1.002 | nan | nan | nan |  | same |
| q32 | 69.19 | 68.69 | 0.993 | 180.81 | 181.38 | 1.003 | 319.60 | 321.41 | 1.006 | nan | nan | nan |  | same |
| q33 | 216.50 | 211.18 | 0.975 | 469.00 | 376.67 | 0.803 | 910.37 | 807.20 | 0.887 | nan | nan | nan |  | same |
| q34 | 43.31 | 42.99 | 0.993 | 96.43 | 96.14 | 0.997 | 184.89 | 183.52 | 0.993 | nan | nan | nan |  | same |
| q35 | 410.91 | 42.99 | 0.105 | 2413.37 | 96.23 | 0.040 | 3238.69 | 182.62 | 0.056 | nan | nan | nan |  | same |
| q36 | 149.45 | 146.72 | 0.982 | 216.86 | 215.22 | 0.992 | 521.42 | 511.41 | 0.981 | nan | nan | nan |  | same |
| q37 | 36.18 | 35.88 | 0.992 | 73.06 | 72.50 | 0.992 | 146.74 | 146.88 | 1.001 | nan | nan | nan |  | same |
| q38 | 35.14 | 34.62 | 0.985 | 73.36 | 73.09 | 0.996 | 148.24 | 146.13 | 0.986 | nan | nan | nan |  | same |
| q39 | 15.14 | 14.85 | 0.981 | 29.02 | 28.76 | 0.991 | 59.82 | 58.97 | 0.986 | nan | nan | nan |  | same |
| q40 | 151.47 | 157.40 | 1.039 | 461.18 | 466.83 | 1.012 | 772.26 | 790.21 | 1.023 | nan | nan | nan |  | same |
| q41 | 17.39 | 18.28 | 1.051 | 42.08 | 39.44 | 0.937 | 78.24 | 76.10 | 0.973 | nan | nan | nan |  | same |
| q42 | 13.30 | 13.72 | 1.031 | 26.31 | 24.74 | 0.940 | 53.13 | 52.58 | 0.990 | nan | nan | nan |  | same |
| q43 | 28.35 | 27.89 | 0.984 | 42.28 | 41.29 | 0.976 | 99.99 | 98.17 | 0.982 | nan | nan | nan |  | same |

```
totals over all 43: warm 10216.3 vs 9474.6 (0.927); first run 25986.5 vs 22995.4 (0.885); 3-run 46632.1 vs 42126.5 (0.903)
answer_mismatches none
order_differences none
gen0 warm_gmean=0.940 warm_worst=1.468 worst5 [(1.468, 'q8'), (1.174, 'q28'), (1.136, 'q2'), (1.051, 'q41'), (1.039, 'q40')]
```

Splayed: scan-only warm gmean 0.730 (worst q2 5.288), first-run gmean 1.007
(worst q2 4.734), cold gmean 0.394 (worst q39 2.085); all-43 totals warm
1.055, first run 2.453 (PI after P1: 2.99), 3-run 1.634.  Parted: scan-only
warm gmean 0.535 (worst q18 5.676), first-run gmean 0.523 (worst q8 3.223),
cold gmean 0.336 (worst q33 1.225); all-43 totals warm 0.887, first run 1.301,
3-run 1.122.  Generation 0 against the base binary: splayed warm gmean 0.982
(worst q27 1.170), parted 0.943 (worst q28 1.168), ragged 0.940 (worst q8
1.468); q16 parted first run 144.5 -> 100.7 ms (carry 2).
Encodings: the only key with DELTA vectors is UserID (splayed 97 of 9 766
vectors delta_bp, 9 526 rle; parted 89).  Decoded bytes per run over the plain
column (probe on the release library, gen 2): q16 UserID 1.052x splayed,
1.055x parted; q17 UserID and SearchPhrase 2.059x splayed, 2.058x parted;
q19 (splayed) the same 2.059x;
q2 AdvEngineID 0.928x splayed; q8 AdvEngineID 1.998x parted; q18 parted 1.0x
of both keys per run.
Materialized columns by class (gen 2): distinct counts q9 q10 (q6 parted);
file-domain SYM compares q11-q15, q31 q32, q37 q38; LIKE q21-q23; strlen q28
q29; whole-row top-k gathers q24-q27 (104 columns splayed, 105 parted); q30
sets ResolutionWidth; computed key q19 (EventTime); conjunction gathers
q37-q43; P3-deferred q18 (splayed) and q39.  Not explained by these classes:
parted q11, q12, q14 and q23 materialize all 105 columns.

Gate (failing clauses, worst five, ratio gen2/gen0 or p2/base):
- splayed warm (scan-only): q2 5.288, q8 3.585, q17 1.284, q7 1.119, q33 1.116
- splayed cold: q39 2.085, q7 1.770, q4 1.220, q1 1.018, q33 0.992
- splayed first: q2 4.734, q18 2.729, q39 2.661, q8 1.979, q7 1.660
- splayed materializing warm: q20 2.888, q42 2.611, q15 1.411, q6 1.409, q5 1.259
- parted warm (scan-only): q18 5.676, q8 3.020, q2 1.494, q17 1.290, q33 1.144
- parted cold: q33 1.225 (only one over 1.10)
- parted first: q8 3.223, q18 1.865, q39 1.642, q17 1.262, q33 1.113
- parted materializing warm: q31 2.627, q32 2.477, q42 1.716, q19 1.638, q15 1.386
- gen0 splayed warm: q27 1.170, q25 1.078, q6 1.070, q4 1.046, q28 1.046
- gen0 parted warm: q28 1.168, q1 1.069, q21 1.014, q31 1.010, q23 1.008
- gen0 ragged warm: q8 1.468, q28 1.174, q2 1.136, q41 1.051, q40 1.039

```
GATE (strict) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL scan_only_zero=FAIL first_total=PASS] parted[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL scan_only_zero=FAIL] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (strict): FAIL
GATE (P3-deferred q18 q39 exempt) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL scan_only_zero=PASS first_total=PASS] parted[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL scan_only_zero=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (P3-deferred q18 q39 exempt): FAIL
```
