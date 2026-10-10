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

### Gate noise floor (A/A evidence, P2 gate)

A/A = the base binary against itself under the gate protocol (five
alternated rounds, minimum-round warm).  Measured on the 10M store:

| class | queries | b/a | abs gap | round-median spread |
|---|---|---|---|---|
| statistics answers | splayed q1, q4, q5, q6, q7 (0.2-1.4 us) | 0.94-1.02 | <= 0.02 us | up to +36% (q6 0.43-0.56 us) |
| ~0.02 ms | q30 (18.7 us) | 1.009 | 0.16 us | 18.7-33.6 us |
| ~1 ms | q25, q27, q20, q2 (0.86-1.15 ms) | 1.002-1.041 | <= 35 us | up to 1.07 ms vs 0.86 min |
| >= 4 ms | parted q2, q8, q20, q39, q41, q42, q43, q18; splayed q3, q18, q39, q41, q42 | 0.984-1.011 | n/a | n/a |

Gate rule (p2_summary.py): a per-query clause passes at ratio <= its bound
(1.05 warm, 1.10 first and cold), or when both medians are < 1 ms and the
gap is <= 0.05 ms (about 1.4x the largest sub-ms A/A gap, 35 us; it does not
hide the q25/q27 rows: on the old P2 gen-0 splayed arm q25 is 0.95 -> 1.03
ms and q27 0.93 -> 1.08 ms, which fail because p2 crosses 1 ms, so the
sub-ms clause does not apply (the PI section's 15-round confirm of q25,
1.056 on a different head/dev pair, is a separate measurement and is not
evidence about this arm).  Geometric means cover only queries whose base
median is >= 1 ms; the excluded queries are printed per column
(`below_1ms_excluded_from_warm_gmean`, `below_1ms_excluded_from_first_gmean`,
`below_1ms_excluded_from_cold_gmean`), because a +-6% ratio wobble on
0.2 us answers moves a gmean and means nothing.  Above 4 ms A/A stays within
+-1.1%, so 1.05 keeps a 4x margin.

Query classes are taken from the materialization the run measured (the
`mat` rows), not a fixed list: q5, q20, q36 do not materialize; splayed q18
and q39, and parted q39, do.  A SCAN_ONLY-list query that materializes stays
gated as scan-only (warm, first, cold) and fails `no_unexpected_materialization`
unless it is in P3_DEFERRALS for that arm; a missing `mat` row fails the
same field.  The plan's pre-run SCAN_ONLY list is kept as a cross-check and
its disagreements are printed.  Expected P3 deferrals are a data table
(`P3_DEFERRALS`) in p2_summary.py; the output has GATE (strict), GATE
(P3-exempt) and the exempt list with each exempted query's measured numbers.
Each deferral has a `max_ratio` per arm (about 1.5x its measured ratio, set
from the gate re-run below); above it the exemption lapses and the clause
fails.

The 1.10 bound on first and cold is a per-query tolerance for cold and
first-run noise.  The spec's "cold is faster" requirement is carried by the
aggregate: the cold geometric mean (queries with a cold base median >= 1 ms)
must be < 1.00.  Geometric-mean filters use each column's own base median.

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

Gate lines of this run as printed by the current p2_summary.py (noise floor,
measured classes, the first P3_DEFERRALS table; the first run's own printout
used an older clause set):

```
GATE (strict) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=FAIL first_total=PASS] parted[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=FAIL] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=FAIL cold=PASS first=FAIL materializing_warm=FAIL no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (P3-exempt): FAIL
```


## P2 gate (re-run)

Re-run of the P2 gate after the gate fix tasks 17a-17h, on HEAD a09a54eb
(release build, sha256 4b72499b44ed) against the base 80a28e49 (release
build from `git archive`, sha256 c204703a62de).  Method, loads, arms and
store sizes are as in "P2 range reads" above: `p2_bench.sh all` as
committed, 5 alternated rounds, 3 cold trials per query (fadvise eviction),
full answers compared; the ragged partition 9 keeps 40 columns.  Raw data:
/home/hetoku/data/tmp/p2_rerun/p2.tsv (summary.txt beside it).  The machine
was otherwise idle (19:13-21:05).

Sizes are unchanged (g0 5 948 677 939 B, g2 3 307 128 731 B; p0
4 807 844 987 B, p2 2 072 366 907 B).  Peak RSS over all 43 queries: g0
5 111 620 kB, g2 8 099 412 kB (x1.58; the q24-q27 whole-row top-k
gathers stay cached); p0 15 208 624 kB, p2 15 875 328 kB (x1.04).
Answers: identical on all five arms (0 mismatches, 0 order differences).

### Gate lines

```
GATE (strict) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=FAIL first_total=PASS] parted[answers=PASS warm=FAIL cold=PASS first=FAIL materializing_warm=FAIL no_unexpected_materialization=FAIL] gen0-splayed[answers=PASS warm=PASS] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=FAIL cold=PASS first=FAIL materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=FAIL cold=PASS first=FAIL materializing_warm=FAIL no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=PASS] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (P3-exempt): FAIL
```

**The P2 gate fails in both variants.**  Generation 0 now passes on every
arm (splayed warm gmean 0.972, parted 0.772, ragged 0.759; q16 parted first
run 146.4 -> 100.8 ms), as do answers, no_unexpected_materialization,
splayed first_total (1.836 vs PI's 2.99) and both cold clauses with the
deferrals.  The P3-exempt failures:

| clause | query | a -> b (ms) | ratio | deferral |
|---|---|---|---|---|
| splayed warm (scan-only) | q33 | 203.29 -> 226.35 | 1.113 | none |
| splayed warm (scan-only) | q34 | 39.03 -> 41.10 | 1.053 | none |
| splayed first (scan-only) | q2 | 1.70 -> 5.49 | 3.219 | q2 has a warm deferral only |
| parted warm (scan-only) | q33 | 211.41 -> 227.61 | 1.077 | none |
| parted first (scan-only) | q8 | 8.33 -> 11.87 | 1.424 | q8 has a warm deferral only |
| parted materializing warm | q32 | 68.71 -> 73.86 | 1.075 | none |

No other query fails a clause after the exemptions.  Below the bound but
close: splayed q35 warm 1.048, parted q31 warm 1.033.  Every failing ratio
reproduces in all five rounds: splayed q33 g0 203.3-205.3 against g2
226.4-230.8 ms; q34 39.0-39.3 against 41.1-41.4 ms; q2 first run 1.64-1.82
against 5.06-5.60 ms.

### What the fixes moved (first gate run -> re-run, ratio gen2/gen0)

- Splayed warm: q2 5.288 -> 1.086 (0.66 ms), q8 3.585 -> 1.424, q17
  1.284 -> 1.016, q20 2.888 -> 0.633, q42 2.611 -> 1.664, q15 1.411 ->
  1.023, q19 1.180 -> 1.031.  q33 1.116 -> 1.113 (unchanged).
- Parted warm: q18 5.676 -> 1.088, q8 3.020 -> 1.060, q2 1.494 -> 1.002,
  q31 2.627 -> 1.033, q32 2.477 -> 1.075, q19 1.638 -> 1.027, q42 1.716 ->
  1.237, q36 1.257 -> 1.002.  q33 1.144 -> 1.077.
- Parted q11, q12, q14 and q23 now gather only their own columns (2-4
  instead of 105); p0 warm q11 357 -> 24 ms, q14 1 117 -> 102 ms; their p2
  first runs are 1.10-1.28x p0 instead of 1.7-4.7x.
- First runs: splayed q24-q27 790-830 -> 450-484 ms (still 14-105x g0: the
  whole-row top-k gather materializes 100 columns); all-43 first-run total
  2.453 -> 1.836.
- Generation 0 against the base: parted q28 1.168 -> 0.983, ragged q28
  1.174 -> 0.990, ragged q8 1.468 -> 0.906, ragged q2 1.136 -> 0.976,
  splayed q25 / q27 0.874 / 0.932, splayed q28 0.986.

### Profiles of the worst failures

Profiled on recreated splayed stores (deleted afterwards), the gate's p2
binary, perf without call graphs.

- **Splayed q33** (`by {WatchID ClientIP}`, count, sum, avg, `desc: c
  take: 10`; 25 runs).  g2 takes the fused radix pass that 17c admitted
  for 32-byte records; g0 keeps the two-pass radix over the plain columns.
  Radix samples: g0 96.1 K (agg_radix_group_fn 30.8%, scatter_fn 23.2%,
  count_fn 6.9%); g2 107.9 K (agg_radix_group_fused_fn 36.9%,
  agg_radix_chunk_fused 14.9%, agg_radix_stage_block 9.2%,
  fused_totals_fn 1.1%), so +12% in the radix itself.  Decode is small
  (dec_fast_runs 2.3%), and memmove is about the same on both (12-14%).  The
  residual is the fused route's staging copy and a slower group pass over
  chunk-major partitions, not decoding.  Parted q33 (1.077) has the same
  shape.
- **Splayed q2 first run** (`count where AdvEngineID != 0`; 1.6 -> 5.5
  ms; run 2 is 0.8 ms on both).  CPU and page faults are about equal over
  the process (g0 333.7 ms task-clock and 36 073 faults; g2 342.5 ms and
  35 675).  The difference is one kernel hotspot (+4 951 samples over 60
  processes; the symbol is hidden by kptr_restrict) and dec_fast_runs
  (+1 111).  On g2 each pool worker creates its 32 MB buddy arena
  (`madvise(MADV_HUGEPAGE)` on 8 arenas, seen in strace) on its first decode
  and faults in fresh zeroed huge pages; g0's count reads the mapped plain
  column in place and allocates nothing in the workers.  This is a fixed
  first-parallel-decode cost per process (about 4 ms), not per query.  By
  the same reading, parted q8's first run (+3.5 ms) is the same fixed
  cost.  This was inferred and not profiled separately.
- **Splayed q34** (`by URL`, count, `take: 10`): g2 adds the URL key decode
  (dec_fast 9.7% of samples); agg_dense_partition_count_rows drops on g2,
  and the net is +5%.

### Remaining P3 deferrals (P3_DEFERRALS, max_ratio about 1.5x the re-run ratio, per arm)

```
q2/A/warm[run-level compute (count over RLE runs)]: 0.61 -> 0.66 ms (x1.086) EXEMPTED
q8/A/warm[run-level compute (group over RLE runs)]: 3.55 -> 5.05 ms (x1.424) EXEMPTED
q8/B/warm[run-level compute (group over RLE runs)]: 6.28 -> 6.66 ms (x1.060) EXEMPTED
q18/A/first[row-list gather (first-n keys published, P3 path 2)]: 13.46 -> 26.70 ms (x1.983) EXEMPTED
q18/B/warm[row-list gather (parted first-n compact build, P3)]: 17.06 -> 18.56 ms (x1.088) EXEMPTED
q39/A/first[selection gather (P3 path 2)]: 14.55 -> 34.94 ms (x2.402) EXEMPTED
q39/B/first[selection gather (P3 path 2)]: 27.30 -> 41.04 ms (x1.503) EXEMPTED
q39/A/cold[selection gather reads whole columns cold (P3)]: 38.04 -> 81.94 ms (x2.154) EXEMPTED
q42/A/warm[point gathers of the selected keys (P3)]: 4.52 -> 7.53 ms (x1.664) EXEMPTED
q42/B/warm[point gathers of the selected keys (P3)]: 13.21 -> 16.34 ms (x1.237) EXEMPTED
q24/A/rss[top-k output gather RSS]: arm peak-RSS 5111620 -> 8099412 kB (x1.58, max 2.4) not gated
q25/A/rss[top-k output gather RSS]: arm peak-RSS 5111620 -> 8099412 kB (x1.58, max 2.4) not gated
q26/A/rss[top-k output gather RSS]: arm peak-RSS 5111620 -> 8099412 kB (x1.58, max 2.4) not gated
q27/A/rss[top-k output gather RSS]: arm peak-RSS 5111620 -> 8099412 kB (x1.58, max 2.4) not gated
```

Removed because their clauses now pass: q2 parted warm, q18 splayed warm,
q18 parted first, q39 warm (both loads), q39 parted cold, and the parted
q24-q27 RSS rows (parted peak RSS x1.04).

### Per-query tables (re-run)

#### arm A: splayed g0 vs g2 (ms; ratio = g2/g0)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.000 | 0.01 | 0.01 | 0.949 | 0.01 | 0.01 | 0.947 | 0.01 | 0.01 | 1.599 | [] | same |
| q2 | 0.61 | 0.66 | 1.086 | 1.70 | 5.49 | 3.219 | 3.26 | 6.89 | 2.111 | 38.51 | 6.16 | 0.160 | [] | same |
| q3 | 3.79 | 0.00 | 0.001 | 5.74 | 0.04 | 0.007 | 13.40 | 0.05 | 0.004 | 94.24 | 0.04 | 0.000 | [] | same |
| q4 | 0.00 | 0.00 | 1.128 | 0.03 | 0.03 | 1.214 | 0.03 | 0.03 | 1.206 | 0.02 | 0.03 | 1.551 | [] | same |
| q5 | 0.00 | 0.00 | 1.303 | 0.03 | 0.03 | 1.030 | 0.03 | 0.03 | 1.033 | 0.03 | 0.03 | 0.989 | [] | same |
| q6 | 0.00 | 0.00 | 1.201 | 0.03 | 0.03 | 0.881 | 0.03 | 0.03 | 0.889 | 0.04 | 0.03 | 0.981 | [] | same |
| q7 | 0.00 | 0.00 | 1.103 | 0.02 | 0.04 | 1.766 | 0.02 | 0.04 | 1.652 | 0.02 | 0.04 | 1.680 | [] | same |
| q8 | 3.55 | 5.05 | 1.424 | 9.44 | 9.87 | 1.046 | 16.69 | 20.11 | 1.205 | 41.36 | 10.76 | 0.260 | [] | same |
| q9 | 75.06 | 74.98 | 0.999 | 114.34 | 125.02 | 1.093 | 266.42 | 278.85 | 1.047 | 698.31 | 155.01 | 0.222 | [RegionID UserID] | same |
| q10 | 68.90 | 72.07 | 1.046 | 104.68 | 120.58 | 1.152 | 242.78 | 267.98 | 1.104 | 427.77 | 158.53 | 0.371 | [RegionID UserID] | same |
| q11 | 16.12 | 16.04 | 0.995 | 25.11 | 40.65 | 1.619 | 57.67 | 73.03 | 1.266 | 255.95 | 61.36 | 0.240 | [UserID MobilePhoneModel] | same |
| q12 | 16.18 | 16.24 | 1.004 | 25.81 | 44.30 | 1.717 | 59.76 | 77.41 | 1.295 | 291.81 | 65.94 | 0.226 | [UserID MobilePhone MobilePhoneModel] | same |
| q13 | 32.01 | 32.08 | 1.002 | 41.77 | 48.69 | 1.166 | 106.63 | 113.51 | 1.065 | 112.69 | 63.89 | 0.567 | [SearchPhrase] | same |
| q14 | 62.67 | 63.03 | 1.006 | 88.97 | 105.38 | 1.184 | 217.83 | 236.32 | 1.085 | 315.91 | 140.80 | 0.446 | [UserID SearchPhrase] | same |
| q15 | 36.50 | 37.35 | 1.023 | 48.21 | 57.46 | 1.192 | 122.73 | 133.70 | 1.089 | 155.79 | 102.64 | 0.659 | [SearchPhrase] | same |
| q16 | 69.84 | 67.42 | 0.965 | 94.34 | 87.91 | 0.932 | 235.40 | 226.02 | 0.960 | 255.81 | 162.48 | 0.635 | [] | same |
| q17 | 92.86 | 94.32 | 1.016 | 128.91 | 124.79 | 0.968 | 316.31 | 316.21 | 1.000 | 474.95 | 254.52 | 0.536 | [] | same |
| q18 | 6.85 | 6.86 | 1.000 | 13.46 | 26.70 | 1.983 | 27.28 | 40.65 | 1.490 | 663.98 | 61.71 | 0.093 | [UserID SearchPhrase] | same |
| q19 | 138.42 | 142.69 | 1.031 | 210.43 | 211.84 | 1.007 | 490.20 | 502.86 | 1.026 | 674.62 | 439.78 | 0.652 | [EventTime] | same |
| q20 | 1.14 | 0.72 | 0.633 | 6.76 | 5.80 | 0.859 | 9.42 | 7.38 | 0.784 | 57.72 | 16.35 | 0.283 | [] | same |
| q21 | 41.72 | 41.77 | 1.001 | 45.25 | 53.08 | 1.173 | 130.18 | 137.60 | 1.057 | 77.09 | 92.35 | 1.198 | [URL] | same |
| q22 | 27.55 | 27.50 | 0.998 | 32.10 | 58.67 | 1.828 | 87.85 | 114.60 | 1.304 | 185.06 | 133.91 | 0.724 | [UserID URL SearchPhrase] | same |
| q23 | 48.77 | 48.92 | 1.003 | 60.42 | 92.66 | 1.534 | 159.54 | 192.46 | 1.206 | 492.28 | 204.24 | 0.415 | [Title UserID URL SearchPhrase] | same |
| q24 | 29.30 | 29.43 | 1.005 | 34.47 | 483.77 | 14.036 | 93.41 | 542.82 | 5.811 | 618.09 | 1229.60 | 1.989 | <100 columns> | same |
| q25 | 0.94 | 0.95 | 1.011 | 4.46 | 451.93 | 101.392 | 6.44 | 454.06 | 70.496 | 305.32 | 1206.99 | 3.953 | <100 columns> | same |
| q26 | 10.82 | 10.80 | 0.998 | 15.31 | 459.17 | 29.984 | 37.01 | 481.05 | 12.997 | 585.28 | 1227.12 | 2.097 | <100 columns> | same |
| q27 | 0.98 | 0.90 | 0.919 | 4.27 | 450.00 | 105.411 | 6.42 | 451.97 | 70.425 | 302.35 | 1207.00 | 3.992 | <100 columns> | same |
| q28 | 80.93 | 66.20 | 0.818 | 83.14 | 80.92 | 0.973 | 246.00 | 215.26 | 0.875 | 219.68 | 120.88 | 0.550 | [URL] | same |
| q29 | 349.56 | 349.14 | 0.999 | 436.06 | 445.20 | 1.021 | 1136.91 | 1146.86 | 1.009 | 512.71 | 492.02 | 0.960 | [Referer] | same |
| q30 | 0.02 | 0.03 | 1.709 | 2.81 | 5.36 | 1.904 | 2.88 | 5.43 | 1.884 | 56.07 | 11.36 | 0.203 | [ResolutionWidth] | same |
| q31 | 51.88 | 51.59 | 0.994 | 69.69 | 94.36 | 1.354 | 173.70 | 198.10 | 1.140 | 317.84 | 139.08 | 0.438 | [ClientIP IsRefresh ResolutionWidth SearchEngineID SearchPhrase] | same |
| q32 | 58.95 | 58.87 | 0.999 | 85.08 | 107.21 | 1.260 | 203.40 | 226.66 | 1.114 | 442.13 | 283.20 | 0.641 | [ClientIP IsRefresh ResolutionWidth SearchPhrase] | same |
| q33 | 203.29 | 226.35 | 1.113 | 365.04 | 373.20 | 1.022 | 776.52 | 833.91 | 1.074 | 593.34 | 566.05 | 0.954 | [] | same |
| q34 | 39.03 | 41.10 | 1.053 | 69.48 | 73.98 | 1.065 | 148.04 | 156.24 | 1.055 | 141.98 | 110.62 | 0.779 | [] | same |
| q35 | 39.29 | 41.17 | 1.048 | 69.90 | 73.94 | 1.058 | 151.71 | 157.04 | 1.035 | 138.49 | 110.54 | 0.798 | [] | same |
| q36 | 64.06 | 64.21 | 1.002 | 88.60 | 81.73 | 0.922 | 217.62 | 213.36 | 0.980 | 160.18 | 126.43 | 0.789 | [] | same |
| q37 | 16.56 | 16.54 | 0.999 | 27.10 | 44.94 | 1.659 | 60.88 | 78.54 | 1.290 | 43.66 | 87.74 | 2.010 | [EventDate URL IsRefresh DontCountHits] | same |
| q38 | 14.91 | 14.91 | 1.000 | 24.54 | 42.29 | 1.724 | 55.71 | 72.18 | 1.296 | 43.25 | 81.69 | 1.889 | [Title EventDate IsRefresh DontCountHits] | same |
| q39 | 7.16 | 7.23 | 1.010 | 14.55 | 34.94 | 2.402 | 28.97 | 49.55 | 1.710 | 38.04 | 81.94 | 2.154 | [EventDate URL IsRefresh IsLink IsDownload] | same |
| q40 | 58.91 | 58.46 | 0.992 | 90.61 | 127.01 | 1.402 | 210.68 | 245.31 | 1.164 | 118.32 | 229.58 | 1.940 | [EventDate CounterID URL Referer IsRefresh TraficSourceID SearchEngineID AdvEngineID] | same |
| q41 | 5.79 | 5.75 | 0.993 | 13.17 | 27.12 | 2.059 | 25.41 | 39.19 | 1.542 | 74.77 | 48.43 | 0.648 | [EventDate CounterID IsRefresh TraficSourceID] | same |
| q42 | 4.52 | 7.53 | 1.664 | 11.14 | 27.83 | 2.498 | 20.54 | 43.31 | 2.109 | 39.07 | 39.70 | 1.016 | [EventDate CounterID IsRefresh DontCountHits] | same |
| q43 | 8.95 | 8.90 | 0.994 | 18.32 | 41.98 | 2.291 | 37.10 | 60.29 | 1.625 | 41.87 | 146.39 | 3.497 | [EventTime EventDate CounterID IsRefresh DontCountHits] | same |

```
totals over all 43: warm 1788.4 vs 1807.8 (1.011); first run 2585.3 vs 4746.0 (1.836); 3-run 6202.8 vs 8416.9 (1.357)
answer_mismatches none
order_differences none
scan_only(gated) n=17; gmeans over base>=1ms (per column): warm n=11 0.512, first n=12 0.821, cold n=12 0.270; worst warm=1.424
below_1ms_excluded_from_warm_gmean q1 q2 q4 q5 q6 q7
below_1ms_excluded_from_first_gmean q1 q4 q5 q6 q7
below_1ms_excluded_from_cold_gmean q1 q4 q5 q6 q7
mat rows missing none
scan_only_list_but_materializing q18 q39
measured_materializing q9(2 cols) q10(2 cols) q11(2 cols) q12(3 cols) q13(1 cols) q14(2 cols) q15(1 cols) q18(2 cols) q19(1 cols) q21(1 cols) q22(3 cols) q23(4 cols) q24(100 cols) q25(100 cols) q26(100 cols) q27(100 cols) q28(1 cols) q29(1 cols) q30(1 cols) q31(5 cols) q32(4 cols) q37(4 cols) q38(4 cols) q39(5 cols) q40(8 cols) q41(4 cols) q42(4 cols) q43(5 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q6(measured scan, list other) q18(measured materializing, list scan) q20(measured scan, list other) q36(measured scan, list other) q39(measured materializing, list scan)
worst5 warm [(1.424, 'q8'), (1.303, 'q5'), (1.201, 'q6'), (1.128, 'q4'), (1.113, 'q33')]
worst5 cold [(2.154, 'q39'), (1.68, 'q7'), (1.599, 'q1'), (1.551, 'q4'), (0.989, 'q5')]
worst5 first [(3.219, 'q2'), (2.402, 'q39'), (1.983, 'q18'), (1.766, 'q7'), (1.214, 'q4')]
worst5 materializing_warm [(1.709, 'q30'), (1.664, 'q42'), (1.046, 'q10'), (1.031, 'q19'), (1.023, 'q15')]
first_run_total_ratio 1.836 (PI after P1: 2.99)
pi_row q41 warm 0.993 first 2.059 3run 1.542
pi_row q42 warm 1.664 first 2.498 3run 2.109
```

#### arm B: parted p0 vs p2 (ms; ratio = p2/p0)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 0.939 | 0.01 | 0.01 | 1.021 | 0.01 | 0.01 | 1.013 | 0.01 | 0.01 | 1.641 | [] | same |
| q2 | 6.59 | 6.61 | 1.002 | 17.02 | 16.66 | 0.979 | 30.59 | 29.92 | 0.978 | 46.71 | 16.12 | 0.345 | [] | same |
| q3 | 526.01 | 5.80 | 0.011 | 546.45 | 10.36 | 0.019 | 1605.41 | 22.22 | 0.014 | 572.75 | 14.51 | 0.025 | [] | same |
| q4 | 273.95 | 7.40 | 0.027 | 324.65 | 12.19 | 0.038 | 875.53 | 27.25 | 0.031 | 364.44 | 31.48 | 0.086 | [] | same |
| q5 | 275.75 | 0.00 | 0.000 | 318.24 | 0.03 | 0.000 | 879.49 | 0.03 | 0.000 | 367.60 | 0.03 | 0.000 | [] | same |
| q6 | 340.90 | 340.28 | 0.998 | 9631.26 | 9644.03 | 1.001 | 10315.67 | 10322.85 | 1.001 | 9704.46 | 9738.85 | 1.004 | [SearchPhrase] | same |
| q7 | 528.29 | 2.55 | 0.005 | 552.67 | 6.78 | 0.012 | 1614.34 | 12.05 | 0.007 | 584.52 | 6.99 | 0.012 | [] | same |
| q8 | 6.28 | 6.66 | 1.060 | 8.33 | 11.87 | 1.424 | 21.43 | 25.28 | 1.179 | 40.91 | 12.13 | 0.297 | [] | same |
| q9 | 85.49 | 83.04 | 0.971 | 151.33 | 168.06 | 1.111 | 325.67 | 339.87 | 1.044 | 392.07 | 190.61 | 0.486 | [RegionID UserID] | same |
| q10 | 79.28 | 79.92 | 1.008 | 152.67 | 173.23 | 1.135 | 317.84 | 338.26 | 1.064 | 445.44 | 204.18 | 0.458 | [RegionID UserID] | same |
| q11 | 24.22 | 23.35 | 0.964 | 59.87 | 66.57 | 1.112 | 110.25 | 115.41 | 1.047 | 386.31 | 86.32 | 0.223 | [UserID MobilePhoneModel] | same |
| q12 | 26.76 | 25.89 | 0.968 | 61.73 | 71.87 | 1.164 | 115.70 | 123.51 | 1.067 | 442.56 | 91.15 | 0.206 | [UserID MobilePhone MobilePhoneModel] | same |
| q13 | 40.16 | 39.51 | 0.984 | 70.53 | 76.09 | 1.079 | 151.55 | 160.77 | 1.061 | 155.34 | 89.21 | 0.574 | [SearchPhrase] | same |
| q14 | 102.09 | 101.84 | 0.998 | 153.81 | 168.49 | 1.095 | 356.99 | 376.16 | 1.054 | 447.03 | 198.01 | 0.443 | [UserID SearchPhrase] | same |
| q15 | 46.66 | 46.36 | 0.994 | 89.59 | 87.05 | 0.972 | 185.61 | 181.49 | 0.978 | 212.99 | 104.60 | 0.491 | [SearchPhrase] | same |
| q16 | 72.51 | 67.96 | 0.937 | 98.04 | 88.21 | 0.900 | 244.76 | 225.69 | 0.922 | 249.61 | 151.20 | 0.606 | [] | same |
| q17 | 95.54 | 94.71 | 0.991 | 134.48 | 127.27 | 0.946 | 328.35 | 317.25 | 0.966 | 475.44 | 238.70 | 0.502 | [] | same |
| q18 | 17.06 | 18.56 | 1.088 | 83.65 | 33.74 | 0.403 | 118.10 | 71.25 | 0.603 | 338.42 | 60.62 | 0.179 | [] | same |
| q19 | 141.22 | 145.00 | 1.027 | 335.90 | 247.66 | 0.737 | 622.09 | 542.34 | 0.872 | 758.54 | 368.74 | 0.486 | [EventTime] | same |
| q20 | 4.64 | 1.58 | 0.341 | 14.10 | 12.11 | 0.859 | 23.72 | 15.78 | 0.665 | 157.26 | 22.40 | 0.142 | [] | same |
| q21 | 41.61 | 41.81 | 1.005 | 46.89 | 57.16 | 1.219 | 130.39 | 144.36 | 1.107 | 87.97 | 98.27 | 1.117 | [URL] | same |
| q22 | 60.42 | 60.33 | 0.998 | 86.10 | 111.74 | 1.298 | 208.76 | 235.71 | 1.129 | 231.42 | 181.49 | 0.784 | [UserID URL SearchPhrase] | same |
| q23 | 90.09 | 89.70 | 0.996 | 119.09 | 151.84 | 1.275 | 300.76 | 332.59 | 1.106 | 471.79 | 267.51 | 0.567 | [Title UserID URL SearchPhrase] | same |
| q24 | 44.58 | 43.67 | 0.980 | 136.70 | 611.73 | 4.475 | 225.91 | 703.59 | 3.114 | 9627.33 | 1710.92 | 0.178 | <105 columns> | same |
| q25 | 1055.56 | 1049.63 | 0.994 | 1324.40 | 1695.02 | 1.280 | 3459.65 | 3799.65 | 1.098 | 9637.01 | 2802.51 | 0.291 | <105 columns> | same |
| q26 | 1107.19 | 1099.57 | 0.993 | 1467.45 | 1841.83 | 1.255 | 3693.59 | 4043.28 | 1.095 | 9841.52 | 2960.88 | 0.301 | <105 columns> | same |
| q27 | 1070.09 | 1059.40 | 0.990 | 1337.34 | 1705.59 | 1.275 | 3481.39 | 3840.98 | 1.103 | 9685.41 | 2824.07 | 0.292 | <105 columns> | same |
| q28 | 90.95 | 87.22 | 0.959 | 146.37 | 148.83 | 1.017 | 328.87 | 326.29 | 0.992 | 306.51 | 187.99 | 0.613 | [URL] | same |
| q29 | 356.07 | 356.45 | 1.001 | 466.66 | 472.08 | 1.012 | 1185.18 | 1186.91 | 1.001 | 556.14 | 515.04 | 0.926 | [Referer] | same |
| q30 | 259.57 | 261.38 | 1.007 | 278.00 | 286.12 | 1.029 | 817.88 | 834.14 | 1.020 | 285.72 | 297.50 | 1.041 | [ResolutionWidth] | same |
| q31 | 54.99 | 56.81 | 1.033 | 125.08 | 104.90 | 0.839 | 236.97 | 223.06 | 0.941 | 425.79 | 140.07 | 0.329 | [SearchPhrase] | same |
| q32 | 68.71 | 73.86 | 1.075 | 178.93 | 132.71 | 0.742 | 317.34 | 282.16 | 0.889 | 617.89 | 297.80 | 0.482 | [SearchPhrase] | same |
| q33 | 211.41 | 227.61 | 1.077 | 372.78 | 390.88 | 1.049 | 807.99 | 847.37 | 1.049 | 612.81 | 613.61 | 1.001 | [] | same |
| q34 | 42.93 | 44.02 | 1.025 | 95.60 | 78.23 | 0.818 | 181.73 | 166.67 | 0.917 | 179.56 | 117.91 | 0.657 | [] | same |
| q35 | 42.87 | 44.05 | 1.028 | 95.81 | 78.05 | 0.815 | 182.40 | 167.37 | 0.918 | 180.97 | 116.55 | 0.644 | [] | same |
| q36 | 145.14 | 145.50 | 1.002 | 213.77 | 197.79 | 0.925 | 511.69 | 494.86 | 0.967 | 298.14 | 207.53 | 0.696 | [] | same |
| q37 | 35.93 | 34.19 | 0.952 | 71.33 | 83.84 | 1.175 | 143.19 | 153.65 | 1.073 | 224.38 | 127.33 | 0.567 | [EventDate URL IsRefresh DontCountHits] | same |
| q38 | 34.95 | 33.26 | 0.952 | 67.59 | 84.08 | 1.244 | 143.55 | 151.44 | 1.055 | 222.49 | 127.73 | 0.574 | [Title EventDate IsRefresh DontCountHits] | same |
| q39 | 15.03 | 13.32 | 0.886 | 27.30 | 41.04 | 1.503 | 57.34 | 73.05 | 1.274 | 116.25 | 49.94 | 0.430 | [EventDate URL IsRefresh IsLink IsDownload] | same |
| q40 | 116.16 | 116.46 | 1.003 | 423.77 | 364.75 | 0.861 | 658.30 | 601.00 | 0.913 | 788.41 | 457.05 | 0.580 | [EventDate IsRefresh] | same |
| q41 | 17.57 | 17.14 | 0.976 | 39.42 | 58.70 | 1.489 | 74.96 | 97.70 | 1.303 | 265.79 | 229.63 | 0.864 | [EventDate CounterID IsRefresh TraficSourceID URLHash] | same |
| q42 | 13.21 | 16.34 | 1.237 | 23.99 | 45.40 | 1.893 | 50.56 | 81.18 | 1.606 | 192.16 | 165.19 | 0.860 | [EventDate CounterID IsRefresh WindowClientWidth WindowClientHeight DontCountHits] | same |
| q43 | 27.80 | 25.74 | 0.926 | 40.92 | 63.19 | 1.544 | 97.11 | 117.60 | 1.211 | 128.79 | 157.31 | 1.221 | [EventTime EventDate IsRefresh DontCountHits] | same |

```
totals over all 43: warm 7696.2 vs 6094.5 (0.792); first run 19989.6 vs 19827.8 (0.992); 3-run 35538.6 vs 32152.0 (0.905)
answer_mismatches none
order_differences none
scan_only(gated) n=16; gmeans over base>=1ms (per column): warm n=15 0.159, first n=15 0.231, cold n=15 0.142; worst warm=1.088
below_1ms_excluded_from_warm_gmean q1
below_1ms_excluded_from_first_gmean q1
below_1ms_excluded_from_cold_gmean q1
mat rows missing none
scan_only_list_but_materializing q39
measured_materializing q6(1 cols) q9(2 cols) q10(2 cols) q11(2 cols) q12(3 cols) q13(1 cols) q14(2 cols) q15(1 cols) q19(1 cols) q21(1 cols) q22(3 cols) q23(4 cols) q24(105 cols) q25(105 cols) q26(105 cols) q27(105 cols) q28(1 cols) q29(1 cols) q30(1 cols) q31(1 cols) q32(1 cols) q37(4 cols) q38(4 cols) q39(5 cols) q40(2 cols) q41(5 cols) q42(6 cols) q43(4 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q20(measured scan, list other) q36(measured scan, list other) q39(measured materializing, list scan)
worst5 warm [(1.088, 'q18'), (1.077, 'q33'), (1.06, 'q8'), (1.028, 'q35'), (1.025, 'q34')]
worst5 cold [(1.641, 'q1'), (1.001, 'q33'), (0.696, 'q36'), (0.657, 'q34'), (0.644, 'q35')]
worst5 first [(1.503, 'q39'), (1.424, 'q8'), (1.049, 'q33'), (1.021, 'q1'), (0.979, 'q2')]
worst5 materializing_warm [(1.237, 'q42'), (1.075, 'q32'), (1.033, 'q31'), (1.027, 'q19'), (1.008, 'q10')]
pi_row q41 warm 0.976 first 1.489 3run 1.303
pi_row q42 warm 1.237 first 1.893 3run 1.606
```

#### arm C0: splayed g0, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.058 | 0.01 | 0.01 | 1.820 | 0.01 | 0.01 | 1.765 | nan | nan | nan |  | same |
| q2 | 1.11 | 0.63 | 0.567 | 2.01 | 1.67 | 0.833 | 4.32 | 3.11 | 0.719 | nan | nan | nan |  | same |
| q3 | 3.78 | 3.79 | 1.003 | 5.73 | 5.83 | 1.017 | 13.35 | 13.49 | 1.010 | nan | nan | nan |  | same |
| q4 | 0.00 | 0.00 | 1.063 | 0.02 | 0.02 | 1.170 | 0.02 | 0.03 | 1.160 | nan | nan | nan |  | same |
| q5 | 0.00 | 0.00 | 1.087 | 0.02 | 0.03 | 1.258 | 0.02 | 0.03 | 1.268 | nan | nan | nan |  | same |
| q6 | 0.00 | 0.00 | 1.052 | 0.02 | 0.03 | 1.196 | 0.02 | 0.03 | 1.195 | nan | nan | nan |  | same |
| q7 | 0.00 | 0.00 | 0.995 | 0.02 | 0.02 | 1.123 | 0.02 | 0.02 | 1.089 | nan | nan | nan |  | same |
| q8 | 3.49 | 3.53 | 1.012 | 9.08 | 8.80 | 0.969 | 16.27 | 16.07 | 0.988 | nan | nan | nan |  | same |
| q9 | 75.71 | 75.25 | 0.994 | 113.89 | 112.32 | 0.986 | 268.58 | 262.98 | 0.979 | nan | nan | nan |  | same |
| q10 | 69.02 | 69.16 | 1.002 | 97.58 | 97.05 | 0.995 | 237.44 | 238.34 | 1.004 | nan | nan | nan |  | same |
| q11 | 16.13 | 16.17 | 1.002 | 25.15 | 25.09 | 0.998 | 57.83 | 57.83 | 1.000 | nan | nan | nan |  | same |
| q12 | 16.47 | 16.30 | 0.990 | 25.73 | 25.77 | 1.001 | 59.09 | 59.34 | 1.004 | nan | nan | nan |  | same |
| q13 | 31.93 | 32.19 | 1.008 | 41.16 | 41.85 | 1.017 | 105.01 | 106.02 | 1.010 | nan | nan | nan |  | same |
| q14 | 62.81 | 62.57 | 0.996 | 87.56 | 89.78 | 1.025 | 216.46 | 218.50 | 1.009 | nan | nan | nan |  | same |
| q15 | 36.45 | 36.37 | 0.998 | 48.39 | 48.17 | 0.996 | 125.50 | 125.06 | 0.997 | nan | nan | nan |  | same |
| q16 | 69.63 | 70.03 | 1.006 | 93.90 | 95.05 | 1.012 | 236.61 | 235.36 | 0.995 | nan | nan | nan |  | same |
| q17 | 92.95 | 93.00 | 1.001 | 129.64 | 130.41 | 1.006 | 320.42 | 317.92 | 0.992 | nan | nan | nan |  | same |
| q18 | 6.91 | 6.89 | 0.997 | 14.45 | 14.63 | 1.013 | 28.35 | 28.40 | 1.002 | nan | nan | nan |  | same |
| q19 | 137.54 | 136.75 | 0.994 | 210.31 | 207.22 | 0.985 | 490.43 | 491.09 | 1.001 | nan | nan | nan |  | same |
| q20 | 1.16 | 1.13 | 0.977 | 6.78 | 6.87 | 1.013 | 9.36 | 9.79 | 1.045 | nan | nan | nan |  | same |
| q21 | 41.86 | 41.77 | 0.998 | 45.19 | 45.25 | 1.001 | 129.10 | 130.72 | 1.013 | nan | nan | nan |  | same |
| q22 | 27.52 | 27.57 | 1.002 | 32.13 | 32.00 | 0.996 | 87.65 | 87.56 | 0.999 | nan | nan | nan |  | same |
| q23 | 48.78 | 48.88 | 1.002 | 60.45 | 60.51 | 1.001 | 159.52 | 159.40 | 0.999 | nan | nan | nan |  | same |
| q24 | 29.18 | 29.57 | 1.013 | 34.34 | 34.87 | 1.015 | 92.88 | 94.20 | 1.014 | nan | nan | nan |  | same |
| q25 | 1.03 | 0.90 | 0.874 | 4.42 | 4.44 | 1.004 | 6.46 | 6.62 | 1.024 | nan | nan | nan |  | same |
| q26 | 10.75 | 10.81 | 1.005 | 15.36 | 15.38 | 1.001 | 37.22 | 37.40 | 1.005 | nan | nan | nan |  | same |
| q27 | 0.96 | 0.89 | 0.932 | 4.39 | 4.36 | 0.993 | 6.42 | 6.25 | 0.974 | nan | nan | nan |  | same |
| q28 | 82.86 | 81.72 | 0.986 | 85.43 | 86.03 | 1.007 | 251.42 | 249.28 | 0.991 | nan | nan | nan |  | same |
| q29 | 353.72 | 348.33 | 0.985 | 443.88 | 438.75 | 0.988 | 1157.92 | 1140.94 | 0.985 | nan | nan | nan |  | same |
| q30 | 0.02 | 0.02 | 0.996 | 3.04 | 3.04 | 0.999 | 3.12 | 3.11 | 0.998 | nan | nan | nan |  | same |
| q31 | 52.87 | 51.52 | 0.975 | 70.78 | 69.53 | 0.982 | 177.98 | 173.27 | 0.974 | nan | nan | nan |  | same |
| q32 | 59.35 | 59.38 | 1.000 | 88.91 | 86.25 | 0.970 | 208.12 | 205.19 | 0.986 | nan | nan | nan |  | same |
| q33 | 202.16 | 201.70 | 0.998 | 364.33 | 365.65 | 1.004 | 784.03 | 776.80 | 0.991 | nan | nan | nan |  | same |
| q34 | 39.27 | 39.08 | 0.995 | 70.43 | 69.85 | 0.992 | 149.04 | 150.74 | 1.011 | nan | nan | nan |  | same |
| q35 | 39.36 | 39.26 | 0.998 | 70.41 | 70.55 | 1.002 | 149.31 | 149.38 | 1.000 | nan | nan | nan |  | same |
| q36 | 63.98 | 64.20 | 1.003 | 86.95 | 88.00 | 1.012 | 215.52 | 218.22 | 1.012 | nan | nan | nan |  | same |
| q37 | 17.06 | 16.61 | 0.973 | 27.52 | 27.27 | 0.991 | 61.92 | 60.38 | 0.975 | nan | nan | nan |  | same |
| q38 | 15.99 | 14.90 | 0.932 | 25.23 | 24.68 | 0.978 | 58.10 | 55.08 | 0.948 | nan | nan | nan |  | same |
| q39 | 7.40 | 7.08 | 0.956 | 14.65 | 14.59 | 0.996 | 29.92 | 29.00 | 0.969 | nan | nan | nan |  | same |
| q40 | 59.06 | 58.41 | 0.989 | 90.37 | 90.25 | 0.999 | 210.38 | 211.54 | 1.006 | nan | nan | nan |  | same |
| q41 | 5.82 | 5.66 | 0.972 | 13.42 | 13.49 | 1.005 | 25.68 | 25.27 | 0.984 | nan | nan | nan |  | same |
| q42 | 4.60 | 4.47 | 0.971 | 11.08 | 11.18 | 1.009 | 20.72 | 20.45 | 0.987 | nan | nan | nan |  | same |
| q43 | 9.27 | 8.99 | 0.969 | 18.92 | 18.66 | 0.986 | 38.18 | 37.35 | 0.978 | nan | nan | nan |  | same |

```
totals over all 43: warm 1797.9 vs 1785.5 (0.993); first run 2593.1 vs 2585.2 (0.997); 3-run 6249.7 vs 6211.6 (0.994)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=36)=0.972 warm_worst=1.087 worst5 [(1.087, 'q5'), (1.063, 'q4'), (1.058, 'q1'), (1.052, 'q6'), (1.013, 'q24')]
```

#### arm C1: parted p0, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.090 | 0.01 | 0.01 | 1.125 | 0.01 | 0.01 | 1.111 | nan | nan | nan |  | same |
| q2 | 6.62 | 6.59 | 0.996 | 17.18 | 17.15 | 0.998 | 30.46 | 30.56 | 1.003 | nan | nan | nan |  | same |
| q3 | 534.35 | 528.84 | 0.990 | 554.47 | 547.36 | 0.987 | 1637.37 | 1611.73 | 0.984 | nan | nan | nan |  | same |
| q4 | 273.69 | 277.05 | 1.012 | 323.52 | 321.92 | 0.995 | 880.75 | 875.73 | 0.994 | nan | nan | nan |  | same |
| q5 | 274.70 | 274.50 | 0.999 | 325.77 | 324.56 | 0.996 | 882.26 | 880.37 | 0.998 | nan | nan | nan |  | same |
| q6 | 344.40 | 337.23 | 0.979 | 9648.98 | 9655.06 | 1.001 | 10339.67 | 10350.37 | 1.001 | nan | nan | nan |  | same |
| q7 | 525.73 | 528.95 | 1.006 | 551.41 | 554.90 | 1.006 | 1610.41 | 1620.07 | 1.006 | nan | nan | nan |  | same |
| q8 | 9.16 | 6.29 | 0.687 | 21.57 | 8.00 | 0.371 | 39.84 | 20.79 | 0.522 | nan | nan | nan |  | same |
| q9 | 89.55 | 85.31 | 0.953 | 176.83 | 151.68 | 0.858 | 359.03 | 323.14 | 0.900 | nan | nan | nan |  | same |
| q10 | 86.64 | 78.62 | 0.907 | 204.89 | 153.82 | 0.751 | 383.09 | 313.25 | 0.818 | nan | nan | nan |  | same |
| q11 | 357.31 | 23.88 | 0.067 | 574.30 | 59.12 | 0.103 | 1297.61 | 109.92 | 0.085 | nan | nan | nan |  | same |
| q12 | 357.85 | 26.72 | 0.075 | 572.31 | 60.61 | 0.106 | 1293.21 | 114.46 | 0.089 | nan | nan | nan |  | same |
| q13 | 40.19 | 40.15 | 0.999 | 70.24 | 69.98 | 0.996 | 150.94 | 150.99 | 1.000 | nan | nan | nan |  | same |
| q14 | 1129.54 | 100.67 | 0.089 | 1397.99 | 154.03 | 0.110 | 3668.26 | 362.92 | 0.099 | nan | nan | nan |  | same |
| q15 | 46.65 | 46.71 | 1.001 | 89.22 | 89.09 | 0.999 | 185.76 | 187.00 | 1.007 | nan | nan | nan |  | same |
| q16 | 76.24 | 72.65 | 0.953 | 146.38 | 100.75 | 0.688 | 300.37 | 249.16 | 0.830 | nan | nan | nan |  | same |
| q17 | 103.75 | 96.00 | 0.925 | 205.42 | 134.51 | 0.655 | 413.72 | 329.87 | 0.797 | nan | nan | nan |  | same |
| q18 | 17.17 | 17.09 | 0.995 | 83.45 | 84.35 | 1.011 | 118.02 | 119.23 | 1.010 | nan | nan | nan |  | same |
| q19 | 141.35 | 140.66 | 0.995 | 336.38 | 337.38 | 1.003 | 623.39 | 623.63 | 1.000 | nan | nan | nan |  | same |
| q20 | 4.60 | 4.63 | 1.006 | 14.49 | 14.55 | 1.004 | 24.10 | 24.16 | 1.003 | nan | nan | nan |  | same |
| q21 | 41.63 | 41.67 | 1.001 | 46.63 | 46.61 | 1.000 | 130.32 | 131.78 | 1.011 | nan | nan | nan |  | same |
| q22 | 60.72 | 60.59 | 0.998 | 85.47 | 86.07 | 1.007 | 207.97 | 209.51 | 1.007 | nan | nan | nan |  | same |
| q23 | 93.52 | 90.20 | 0.965 | 203.74 | 117.91 | 0.579 | 394.43 | 298.81 | 0.758 | nan | nan | nan |  | same |
| q24 | 44.43 | 44.38 | 0.999 | 125.11 | 125.24 | 1.001 | 214.87 | 214.58 | 0.999 | nan | nan | nan |  | same |
| q25 | 1069.78 | 1056.88 | 0.988 | 1324.12 | 1307.36 | 0.987 | 3469.64 | 3430.01 | 0.989 | nan | nan | nan |  | same |
| q26 | 1122.13 | 1110.46 | 0.990 | 1471.87 | 1455.46 | 0.989 | 3728.19 | 3679.84 | 0.987 | nan | nan | nan |  | same |
| q27 | 1081.82 | 1065.90 | 0.985 | 1331.94 | 1318.89 | 0.990 | 3500.73 | 3460.25 | 0.988 | nan | nan | nan |  | same |
| q28 | 92.33 | 90.79 | 0.983 | 147.18 | 146.18 | 0.993 | 333.85 | 330.50 | 0.990 | nan | nan | nan |  | same |
| q29 | 361.75 | 359.14 | 0.993 | 472.76 | 463.18 | 0.980 | 1201.24 | 1186.46 | 0.988 | nan | nan | nan |  | same |
| q30 | 265.87 | 262.26 | 0.986 | 279.86 | 278.97 | 0.997 | 824.85 | 819.02 | 0.993 | nan | nan | nan |  | same |
| q31 | 54.98 | 55.85 | 1.016 | 124.60 | 126.63 | 1.016 | 237.76 | 239.78 | 1.008 | nan | nan | nan |  | same |
| q32 | 68.83 | 68.77 | 0.999 | 181.36 | 177.90 | 0.981 | 319.34 | 315.84 | 0.989 | nan | nan | nan |  | same |
| q33 | 217.08 | 211.63 | 0.975 | 461.88 | 370.82 | 0.803 | 902.25 | 800.63 | 0.887 | nan | nan | nan |  | same |
| q34 | 43.35 | 43.01 | 0.992 | 95.46 | 95.52 | 1.001 | 182.01 | 181.82 | 0.999 | nan | nan | nan |  | same |
| q35 | 386.86 | 43.09 | 0.111 | 2293.99 | 95.82 | 0.042 | 3068.90 | 182.94 | 0.060 | nan | nan | nan |  | same |
| q36 | 148.58 | 145.19 | 0.977 | 217.67 | 216.57 | 0.995 | 517.52 | 511.81 | 0.989 | nan | nan | nan |  | same |
| q37 | 36.53 | 36.28 | 0.993 | 69.09 | 69.57 | 1.007 | 142.20 | 142.04 | 0.999 | nan | nan | nan |  | same |
| q38 | 36.39 | 34.92 | 0.960 | 67.76 | 68.35 | 1.009 | 145.06 | 144.49 | 0.996 | nan | nan | nan |  | same |
| q39 | 15.37 | 15.09 | 0.982 | 27.56 | 27.10 | 0.983 | 58.44 | 57.82 | 0.989 | nan | nan | nan |  | same |
| q40 | 117.56 | 116.38 | 0.990 | 422.82 | 420.97 | 0.996 | 659.83 | 659.49 | 0.999 | nan | nan | nan |  | same |
| q41 | 17.60 | 17.70 | 1.006 | 38.81 | 38.69 | 0.997 | 74.59 | 74.48 | 0.998 | nan | nan | nan |  | same |
| q42 | 13.40 | 13.16 | 0.982 | 24.35 | 23.96 | 0.984 | 51.68 | 50.69 | 0.981 | nan | nan | nan |  | same |
| q43 | 28.46 | 27.93 | 0.981 | 41.61 | 40.51 | 0.974 | 99.65 | 96.87 | 0.972 | nan | nan | nan |  | same |

```
totals over all 43: warm 9838.5 vs 7703.8 (0.783); first run 24870.4 vs 19957.1 (0.802); 3-run 44703.6 vs 35516.8 (0.794)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=42)=0.772 warm_worst=1.090 worst5 [(1.09, 'q1'), (1.016, 'q31'), (1.012, 'q4'), (1.006, 'q7'), (1.006, 'q41')]
q16 first run base 146.4 ms, p2 100.7 ms (carry 2)
```

#### arm R: parted ragged, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.033 | 0.01 | 0.01 | 1.157 | 0.01 | 0.01 | 1.141 | nan | nan | nan |  | same |
| q2 | 7.74 | 7.56 | 0.976 | 18.21 | 17.03 | 0.935 | 33.78 | 32.26 | 0.955 | nan | nan | nan |  | same |
| q3 | 570.68 | 264.47 | 0.463 | 593.63 | 286.44 | 0.483 | 1753.58 | 827.68 | 0.472 | nan | nan | nan |  | same |
| q4 | 274.36 | 273.91 | 0.998 | 323.51 | 320.98 | 0.992 | 876.45 | 880.59 | 1.005 | nan | nan | nan |  | same |
| q5 | 275.33 | 273.71 | 0.994 | 324.77 | 321.02 | 0.988 | 883.16 | 877.90 | 0.994 | nan | nan | nan |  | same |
| q6 | 340.49 | 339.03 | 0.996 | 9640.19 | 9661.39 | 1.002 | 10333.78 | 10353.71 | 1.002 | nan | nan | nan |  | same |
| q7 | 522.08 | 523.52 | 1.003 | 551.47 | 559.12 | 1.014 | 1604.08 | 1621.32 | 1.011 | nan | nan | nan |  | same |
| q8 | 9.65 | 8.74 | 0.906 | 22.43 | 10.20 | 0.455 | 41.75 | 27.89 | 0.668 | nan | nan | nan |  | same |
| q9 | 89.46 | 85.23 | 0.953 | 177.06 | 153.64 | 0.868 | 357.59 | 327.12 | 0.915 | nan | nan | nan |  | same |
| q10 | 86.66 | 79.39 | 0.916 | 203.33 | 155.38 | 0.764 | 380.75 | 315.48 | 0.829 | nan | nan | nan |  | same |
| q11 | 372.93 | 23.61 | 0.063 | 644.72 | 57.90 | 0.090 | 1393.45 | 106.95 | 0.077 | nan | nan | nan |  | same |
| q12 | 373.12 | 26.53 | 0.071 | 644.14 | 60.36 | 0.094 | 1389.89 | 114.68 | 0.083 | nan | nan | nan |  | same |
| q13 | 39.85 | 40.12 | 1.007 | 69.70 | 70.43 | 1.010 | 151.89 | 152.43 | 1.004 | nan | nan | nan |  | same |
| q14 | 1181.37 | 100.65 | 0.085 | 1503.10 | 154.65 | 0.103 | 3877.27 | 361.74 | 0.093 | nan | nan | nan |  | same |
| q15 | 46.24 | 46.43 | 1.004 | 88.24 | 89.71 | 1.017 | 184.36 | 186.29 | 1.011 | nan | nan | nan |  | same |
| q16 | 75.98 | 72.39 | 0.953 | 145.43 | 97.06 | 0.667 | 298.59 | 242.06 | 0.811 | nan | nan | nan |  | same |
| q17 | 103.85 | 95.30 | 0.918 | 205.24 | 133.87 | 0.652 | 417.19 | 329.34 | 0.789 | nan | nan | nan |  | same |
| q18 | 17.19 | 17.13 | 0.997 | 83.54 | 83.49 | 0.999 | 118.13 | 119.50 | 1.012 | nan | nan | nan |  | same |
| q19 | 141.25 | 140.96 | 0.998 | 336.46 | 336.21 | 0.999 | 621.70 | 624.61 | 1.005 | nan | nan | nan |  | same |
| q20 | 4.55 | 4.47 | 0.982 | 14.14 | 14.09 | 0.997 | 23.38 | 23.54 | 1.007 | nan | nan | nan |  | same |
| q21 | 41.69 | 41.74 | 1.001 | 46.50 | 46.32 | 0.996 | 130.36 | 129.92 | 0.997 | nan | nan | nan |  | same |
| q22 | 60.45 | 60.37 | 0.999 | 85.22 | 85.55 | 1.004 | 208.61 | 206.46 | 0.990 | nan | nan | nan |  | same |
| q23 | 93.51 | 89.87 | 0.961 | 270.26 | 117.38 | 0.434 | 461.68 | 297.66 | 0.645 | nan | nan | nan |  | same |
| q24 | 44.31 | 44.26 | 0.999 | 188.12 | 167.80 | 0.892 | 282.45 | 260.80 | 0.923 | nan | nan | nan |  | same |
| q25 | 1119.49 | 1107.83 | 0.990 | 1416.34 | 1391.46 | 0.982 | 3658.41 | 3620.41 | 0.990 | nan | nan | nan |  | same |
| q26 | 1171.45 | 1157.76 | 0.988 | 1579.79 | 1537.59 | 0.973 | 3938.50 | 3864.10 | 0.981 | nan | nan | nan |  | same |
| q27 | 1130.19 | 1115.29 | 0.987 | 1437.34 | 1394.53 | 0.970 | 3709.24 | 3636.16 | 0.980 | nan | nan | nan |  | same |
| q28 | 91.64 | 90.70 | 0.990 | 149.64 | 145.44 | 0.972 | 335.88 | 329.34 | 0.981 | nan | nan | nan |  | same |
| q29 | 362.11 | 356.39 | 0.984 | 469.82 | 469.73 | 1.000 | 1199.84 | 1187.90 | 0.990 | nan | nan | nan |  | same |
| q30 | 263.74 | 260.65 | 0.988 | 279.10 | 276.54 | 0.991 | 820.49 | 825.98 | 1.007 | nan | nan | nan |  | same |
| q31 | 55.11 | 54.93 | 0.997 | 124.07 | 124.85 | 1.006 | 236.20 | 239.57 | 1.014 | nan | nan | nan |  | same |
| q32 | 68.55 | 68.69 | 1.002 | 179.32 | 177.13 | 0.988 | 317.66 | 317.15 | 0.998 | nan | nan | nan |  | same |
| q33 | 215.95 | 211.00 | 0.977 | 463.39 | 371.76 | 0.802 | 902.95 | 802.26 | 0.888 | nan | nan | nan |  | same |
| q34 | 43.26 | 43.24 | 0.999 | 95.56 | 96.33 | 1.008 | 182.08 | 183.51 | 1.008 | nan | nan | nan |  | same |
| q35 | 409.93 | 43.03 | 0.105 | 2395.54 | 95.91 | 0.040 | 3220.16 | 187.46 | 0.058 | nan | nan | nan |  | same |
| q36 | 147.75 | 144.95 | 0.981 | 217.38 | 214.19 | 0.985 | 514.99 | 513.51 | 0.997 | nan | nan | nan |  | same |
| q37 | 36.27 | 35.92 | 0.990 | 72.81 | 71.50 | 0.982 | 147.63 | 144.61 | 0.980 | nan | nan | nan |  | same |
| q38 | 35.29 | 34.51 | 0.978 | 73.83 | 72.22 | 0.978 | 146.27 | 143.42 | 0.981 | nan | nan | nan |  | same |
| q39 | 15.14 | 14.91 | 0.985 | 29.21 | 28.23 | 0.966 | 60.14 | 64.27 | 1.069 | nan | nan | nan |  | same |
| q40 | 151.35 | 154.65 | 1.022 | 460.99 | 454.20 | 0.985 | 769.15 | 766.33 | 0.996 | nan | nan | nan |  | same |
| q41 | 17.63 | 17.14 | 0.972 | 40.28 | 38.22 | 0.949 | 76.28 | 72.76 | 0.954 | nan | nan | nan |  | same |
| q42 | 13.27 | 12.72 | 0.958 | 26.50 | 23.80 | 0.898 | 53.40 | 49.56 | 0.928 | nan | nan | nan |  | same |
| q43 | 28.17 | 27.59 | 0.979 | 41.64 | 40.82 | 0.980 | 98.96 | 96.52 | 0.975 | nan | nan | nan |  | same |

```
totals over all 43: warm 10149.0 vs 7611.3 (0.750); first run 25732.0 vs 20024.5 (0.778); 3-run 46212.1 vs 35464.8 (0.767)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=42)=0.759 warm_worst=1.033 worst5 [(1.033, 'q1'), (1.022, 'q40'), (1.007, 'q13'), (1.004, 'q15'), (1.003, 'q7')]
```

