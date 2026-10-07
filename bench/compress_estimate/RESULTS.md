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

