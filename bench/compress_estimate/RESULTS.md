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


## P2 gate (final)

Final run of the P2 gate after the gate fix task 17i, on HEAD df637f04
(release build, sha256 9e4c9efc37a6) against the base 80a28e49 (release
build from `git archive`, sha256 935f88522f32).  Method, loads and arms are
as in "P2 range reads" above: `W=/home/hetoku/data/tmp/p2_final p2_bench.sh
all` as committed, all 43 queries on A, B, C0, C1 and R, 5 alternated
rounds, 3 cold trials per query (fadvise eviction), full answers compared;
the ragged partition 9 keeps 40 columns.  Raw data:
/home/hetoku/data/tmp/p2_final/p2.tsv (summary.txt beside it).  The machine
was otherwise idle (11:50-13:42).

Sizes are unchanged (g0 5 948 677 939 B, g2 3 307 128 731 B; p0
4 807 844 987 B, p2 2 072 366 907 B).  Peak RSS over all 43 queries: g0
5 120 428 kB, g2 8 102 144 kB (x1.58; the q24-q27 whole-row top-k gathers
stay cached); p0 15 253 952 kB, p2 15 812 084 kB (x1.04).  Answers:
identical on all five arms (0 mismatches, 0 order differences).

### Gate lines

```
GATE (strict) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=FAIL first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=FAIL materializing_warm=FAIL no_unexpected_materialization=FAIL] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=FAIL cold=PASS first=PASS materializing_warm=FAIL no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (P3-exempt): FAIL
P3-EXEMPT LIST q8/A/warm[run-level compute (group over RLE runs)]: 3.56 -> 5.06 ms (x1.422) EXEMPTED; q8/A/first[run-level compute (group over RLE runs, first run)]: 8.82 -> 10.21 ms (x1.157) EXEMPTED; q18/A/first[row-list gather (first-n keys published, P3 path 2)]: 13.48 -> 26.42 ms (x1.960) EXEMPTED; q39/A/first[selection gather (P3 path 2)]: 14.60 -> 31.46 ms (x2.155) EXEMPTED; q39/B/first[selection gather (P3 path 2)]: 27.64 -> 40.54 ms (x1.467) EXEMPTED; q39/A/cold[selection gather reads whole columns cold (P3)]: 38.31 -> 81.82 ms (x2.136) EXEMPTED; q42/A/warm[point gathers of the selected keys (P3)]: 4.52 -> 7.54 ms (x1.669) EXEMPTED; q42/B/warm[point gathers of the selected keys (P3)]: 13.12 -> 16.36 ms (x1.246) EXEMPTED; q24/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated; q25/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated; q26/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated; q27/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated
```

**The P2 gate fails in both variants.**  Parted (arm B) now passes every
clause with the deferrals, as do generation 0 on parted and ragged, all
answers, every cold and first clause, no_unexpected_materialization and
splayed first_total (1.816 vs PI's 2.99).  Three clauses fail after the
exemptions:

| clause | query | a -> b (ms) | ratio | per-round range | deferral |
|---|---|---|---|---|---|
| splayed warm (scan-only) | q33 | 202.23 -> 212.53 | 1.051 | g0 202.2-203.8, g2 212.5-215.4 | none |
| splayed materializing warm | q10 | 69.01 -> 72.61 | 1.052 | g0 69.0-69.8, g2 72.6-74.7 | none |
| gen0-splayed warm (C0) | q25 | 0.898 -> 1.001 | 1.115 | base 0.90-1.09, p2 1.00-1.07 | none |

No other clause fails.  Close to a bound: splayed q25 materializing warm
1.049, splayed q35 first 1.036, parted q33 warm 1.030 and cold 1.038.

- **Splayed q33** (`by {WatchID ClientIP}`): 1.113 on the re-run, 1.051 now
  (17i measured 1.041-1.046).  A separate alternated check after the run (7
  rounds, gate-min) gave 1.047, so it sits on the bound.  What remains is
  the fused phase 2 over chunk-major runs (17i report, concern 2).
- **Splayed q10** (`by RegionID`, sum AdvEngineID, count, avg
  ResolutionWidth, count distinct UserID; materializes RegionID and UserID):
  1.046 on the re-run, 1.052 now.  A 7-round alternated re-check gave g2/g0
  1.036-1.072 in every round.  Profile (25 runs in one process, perf without
  call graphs): the two profiles match symbol for symbol (cdpg_buf_par_fn
  6 264 / 6 258 samples, agg_dense_partition_field 4 608 / 4 462) except
  g2's `dec_fast_runs` at 1 753 samples (4.6%), the decode of the RLE-run
  inputs.  That is the whole gap: the same run-level decode cost as q8,
  which P3's run-level compute removes.  It has no deferral.
- **Gen-0 splayed q25** (`where SearchPhrase != "" asc: EventTime take: 10`,
  the SearchPhrase column only): both medians are about 1 ms, and the gap of
  0.10 ms is above the 0.05 ms floor.  The gate takes the best round: base's
  round 3 is 0.898 ms against 1.00-1.09 in the others.  The medians of the
  rounds are 1.012 (base) and 1.029 (p2), 1.017.  A 9-round alternated
  re-check gave base 0.86-1.04 and p2 0.94-1.09 ms (best-round ratio 1.094,
  median 1.009).  The base often lands a fast round and p2 rarely does, so
  this is the gate statistic on a 1 ms query rather than a measured
  regression.  On the re-run it was 0.874.

### What moved since the re-run (ratio gen2/gen0)

- Splayed: q2 first 3.219 -> 0.892, q33 warm 1.113 -> 1.051, q34 warm
  1.053 -> 0.983, q35 warm 1.048 -> 0.986, q2 warm 1.086 -> 1.049 (within
  the bound), q8 first 1.046 -> 1.157 (17i concern 1, now deferred).
- Parted: q8 first 1.424 -> 1.065, q33 warm 1.077 -> 1.030, q32 warm
  1.075 -> 1.038, q8 warm 1.060 -> 1.038, q18 warm 1.088 -> 1.027.
- All-43 splayed first-run total 1.836 -> 1.816.

### Generation-0 warm gmeans (base median >= 1 ms)

| arm | n | gmean | worst |
|---|---|---|---|
| C0 splayed g0, base vs p2 | 35 | 0.977 | 1.115 (q25) |
| C1 parted p0, base vs p2 | 42 | 0.776 | 1.108 (q1, below 1 ms) |
| R parted ragged, base vs p2 | 42 | 0.763 | 1.047 (q30) |

q16 parted first run: base 145.6 ms, p2 98.8 ms.

### P3 deferrals (P3_DEFERRALS, max_ratio about 1.5x this run's ratio, per arm)

```
q8/A/warm[run-level compute (group over RLE runs)]: 3.56 -> 5.06 ms (x1.422) EXEMPTED
q8/A/first[run-level compute (group over RLE runs, first run)]: 8.82 -> 10.21 ms (x1.157) EXEMPTED
q18/A/first[row-list gather (first-n keys published, P3 path 2)]: 13.48 -> 26.42 ms (x1.960) EXEMPTED
q39/A/first[selection gather (P3 path 2)]: 14.60 -> 31.46 ms (x2.155) EXEMPTED
q39/B/first[selection gather (P3 path 2)]: 27.64 -> 40.54 ms (x1.467) EXEMPTED
q39/A/cold[selection gather reads whole columns cold (P3)]: 38.31 -> 81.82 ms (x2.136) EXEMPTED
q42/A/warm[point gathers of the selected keys (P3)]: 4.52 -> 7.54 ms (x1.669) EXEMPTED
q42/B/warm[point gathers of the selected keys (P3)]: 13.12 -> 16.36 ms (x1.246) EXEMPTED
q24/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated
q25/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated
q26/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated
q27/A/rss[top-k output gather RSS]: arm peak-RSS 5120428 -> 8102144 kB (x1.58, max 2.37) not gated
```

- Added by controller ruling: q8 splayed first.  Its cause is the same
  run-level compute as q8's warm deferral, and the first run carries the
  warm gap (g0 8.8, g2 10.2 ms; run 2 onwards 3.6 against 5.1 ms).
- Removed because their clauses now pass: q2 splayed warm (1.049), q8
  parted warm (1.038), q18 parted warm (1.027).
- The reasons by class:
  - run-level compute (q8): P3 computes over RLE runs instead of decoding
    them;
  - row-list gather (q18) and selection gather (q39): P3 path 2;
  - point gathers of the selected keys (q42);
  - top-k output gather RSS (q24-q27 splayed): not gated.

### Per-query tables (final)

#### arm A: splayed g0 vs g2 (ms; ratio = g2/g0)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.000 | 0.01 | 0.01 | 0.930 | 0.01 | 0.01 | 0.872 | 0.01 | 0.01 | 1.730 | [] | same |
| q2 | 0.62 | 0.65 | 1.049 | 1.66 | 1.48 | 0.892 | 3.21 | 2.94 | 0.916 | 38.47 | 3.47 | 0.090 | [] | same |
| q3 | 3.79 | 0.00 | 0.001 | 5.50 | 0.03 | 0.006 | 13.22 | 0.04 | 0.003 | 94.61 | 0.04 | 0.000 | [] | same |
| q4 | 0.00 | 0.00 | 1.013 | 0.02 | 0.03 | 1.247 | 0.03 | 0.03 | 1.236 | 0.03 | 0.03 | 1.218 | [] | same |
| q5 | 0.00 | 0.00 | 1.296 | 0.03 | 0.03 | 0.992 | 0.03 | 0.03 | 0.988 | 0.03 | 0.03 | 0.860 | [] | same |
| q6 | 0.00 | 0.00 | 1.275 | 0.03 | 0.03 | 1.025 | 0.03 | 0.03 | 1.028 | 0.03 | 0.03 | 1.106 | [] | same |
| q7 | 0.00 | 0.00 | 1.175 | 0.02 | 0.03 | 1.731 | 0.02 | 0.04 | 1.631 | 0.02 | 0.03 | 1.522 | [] | same |
| q8 | 3.56 | 5.06 | 1.422 | 8.82 | 10.21 | 1.157 | 16.03 | 20.54 | 1.282 | 41.27 | 10.92 | 0.264 | [] | same |
| q9 | 75.00 | 74.96 | 0.999 | 112.27 | 126.51 | 1.127 | 263.96 | 277.11 | 1.050 | 668.70 | 155.41 | 0.232 | [RegionID UserID] | same |
| q10 | 69.01 | 72.61 | 1.052 | 97.33 | 120.97 | 1.243 | 236.93 | 269.44 | 1.137 | 406.73 | 155.14 | 0.381 | [RegionID UserID] | same |
| q11 | 15.93 | 16.18 | 1.016 | 25.04 | 41.20 | 1.645 | 58.07 | 74.21 | 1.278 | 243.46 | 61.26 | 0.252 | [UserID MobilePhoneModel] | same |
| q12 | 16.22 | 16.38 | 1.010 | 25.42 | 44.56 | 1.753 | 58.98 | 78.87 | 1.337 | 279.43 | 65.47 | 0.234 | [UserID MobilePhone MobilePhoneModel] | same |
| q13 | 31.91 | 31.87 | 0.999 | 41.52 | 47.83 | 1.152 | 107.02 | 112.59 | 1.052 | 114.43 | 63.99 | 0.559 | [SearchPhrase] | same |
| q14 | 62.73 | 62.68 | 0.999 | 89.75 | 105.19 | 1.172 | 219.51 | 234.21 | 1.067 | 306.44 | 148.26 | 0.484 | [UserID SearchPhrase] | same |
| q15 | 36.44 | 37.51 | 1.030 | 47.56 | 57.26 | 1.204 | 122.41 | 133.13 | 1.088 | 155.16 | 101.38 | 0.653 | [SearchPhrase] | same |
| q16 | 69.87 | 63.35 | 0.907 | 96.81 | 88.63 | 0.915 | 236.84 | 218.40 | 0.922 | 241.35 | 166.52 | 0.690 | [] | same |
| q17 | 93.40 | 92.67 | 0.992 | 129.63 | 126.10 | 0.973 | 319.04 | 315.88 | 0.990 | 464.57 | 239.99 | 0.517 | [] | same |
| q18 | 6.84 | 6.87 | 1.004 | 13.48 | 26.42 | 1.960 | 27.55 | 40.27 | 1.461 | 619.26 | 61.93 | 0.100 | [UserID SearchPhrase] | same |
| q19 | 137.16 | 139.33 | 1.016 | 209.23 | 211.98 | 1.013 | 490.38 | 496.67 | 1.013 | 678.65 | 435.57 | 0.642 | [EventTime] | same |
| q20 | 1.16 | 0.73 | 0.631 | 6.92 | 6.12 | 0.885 | 9.50 | 7.73 | 0.813 | 55.12 | 16.65 | 0.302 | [] | same |
| q21 | 41.92 | 42.20 | 1.007 | 45.14 | 50.81 | 1.126 | 129.20 | 135.85 | 1.052 | 76.89 | 93.05 | 1.210 | [URL] | same |
| q22 | 27.53 | 27.56 | 1.001 | 32.48 | 55.17 | 1.699 | 87.56 | 110.69 | 1.264 | 185.74 | 133.34 | 0.718 | [UserID URL SearchPhrase] | same |
| q23 | 48.84 | 48.96 | 1.002 | 60.49 | 88.33 | 1.460 | 159.50 | 187.91 | 1.178 | 430.32 | 204.78 | 0.476 | [Title UserID URL SearchPhrase] | same |
| q24 | 29.48 | 29.38 | 0.997 | 34.61 | 475.67 | 13.744 | 94.09 | 534.77 | 5.684 | 608.60 | 1230.80 | 2.022 | <100 columns> | same |
| q25 | 0.91 | 0.95 | 1.049 | 4.44 | 449.39 | 101.239 | 6.54 | 451.57 | 69.055 | 299.04 | 1214.61 | 4.062 | <100 columns> | same |
| q26 | 10.84 | 10.91 | 1.006 | 15.29 | 449.39 | 29.384 | 37.20 | 472.07 | 12.688 | 574.14 | 1226.63 | 2.136 | <100 columns> | same |
| q27 | 1.00 | 0.96 | 0.957 | 4.30 | 448.34 | 104.187 | 6.38 | 450.45 | 70.637 | 296.98 | 1207.76 | 4.067 | <100 columns> | same |
| q28 | 81.50 | 67.00 | 0.822 | 83.78 | 79.21 | 0.946 | 247.87 | 213.48 | 0.861 | 219.28 | 121.39 | 0.554 | [URL] | same |
| q29 | 348.43 | 349.10 | 1.002 | 437.88 | 442.23 | 1.010 | 1134.78 | 1143.97 | 1.008 | 508.35 | 497.20 | 0.978 | [Referer] | same |
| q30 | 0.02 | 0.02 | 1.010 | 2.82 | 5.33 | 1.889 | 2.89 | 5.39 | 1.868 | 56.54 | 11.45 | 0.203 | [ResolutionWidth] | same |
| q31 | 51.39 | 51.61 | 1.004 | 69.47 | 93.84 | 1.351 | 172.05 | 199.21 | 1.158 | 318.81 | 139.14 | 0.436 | [ClientIP IsRefresh ResolutionWidth SearchEngineID SearchPhrase] | same |
| q32 | 59.27 | 59.28 | 1.000 | 86.52 | 106.58 | 1.232 | 205.99 | 226.04 | 1.097 | 440.77 | 281.46 | 0.639 | [ClientIP IsRefresh ResolutionWidth SearchPhrase] | same |
| q33 | 202.23 | 212.53 | 1.051 | 362.97 | 363.67 | 1.002 | 775.12 | 790.49 | 1.020 | 593.30 | 554.47 | 0.935 | [] | same |
| q34 | 39.36 | 38.68 | 0.983 | 71.49 | 71.60 | 1.002 | 153.17 | 151.87 | 0.992 | 138.09 | 109.68 | 0.794 | [] | same |
| q35 | 39.20 | 38.64 | 0.986 | 69.28 | 71.78 | 1.036 | 148.55 | 149.45 | 1.006 | 138.33 | 109.13 | 0.789 | [] | same |
| q36 | 64.23 | 61.05 | 0.950 | 88.88 | 83.83 | 0.943 | 220.31 | 210.88 | 0.957 | 160.40 | 124.22 | 0.774 | [] | same |
| q37 | 16.53 | 16.53 | 1.000 | 26.88 | 42.39 | 1.577 | 60.93 | 76.70 | 1.259 | 41.99 | 88.78 | 2.114 | [EventDate URL IsRefresh DontCountHits] | same |
| q38 | 14.99 | 14.81 | 0.988 | 24.40 | 40.00 | 1.639 | 55.10 | 70.58 | 1.281 | 39.18 | 81.85 | 2.089 | [Title EventDate IsRefresh DontCountHits] | same |
| q39 | 7.16 | 7.19 | 1.004 | 14.60 | 31.46 | 2.155 | 29.02 | 46.30 | 1.595 | 38.31 | 81.82 | 2.136 | [EventDate URL IsRefresh IsLink IsDownload] | same |
| q40 | 58.39 | 58.61 | 1.004 | 89.89 | 122.42 | 1.362 | 209.12 | 241.66 | 1.156 | 118.56 | 230.26 | 1.942 | [EventDate CounterID URL Referer IsRefresh TraficSourceID SearchEngineID AdvEngineID] | same |
| q41 | 5.84 | 5.69 | 0.975 | 13.48 | 27.31 | 2.026 | 25.59 | 39.16 | 1.530 | 74.93 | 49.16 | 0.656 | [EventDate CounterID IsRefresh TraficSourceID] | same |
| q42 | 4.52 | 7.54 | 1.669 | 10.95 | 28.10 | 2.566 | 20.51 | 43.41 | 2.117 | 37.43 | 40.84 | 1.091 | [EventDate CounterID IsRefresh DontCountHits] | same |
| q43 | 8.97 | 8.93 | 0.995 | 18.45 | 42.46 | 2.301 | 37.30 | 61.30 | 1.644 | 40.57 | 145.80 | 3.594 | [EventTime EventDate CounterID IsRefresh DontCountHits] | same |

```
totals over all 43: warm 1786.2 vs 1779.0 (0.996); first run 2579.5 vs 4683.9 (1.816); 3-run 6201.5 vs 8295.4 (1.338)
answer_mismatches none
order_differences none
scan_only(gated) n=17; gmeans over base>=1ms (per column): warm n=11 0.497, first n=12 0.725, cold n=12 0.259; worst warm=1.422
below_1ms_excluded_from_warm_gmean q1 q2 q4 q5 q6 q7
below_1ms_excluded_from_first_gmean q1 q4 q5 q6 q7
below_1ms_excluded_from_cold_gmean q1 q4 q5 q6 q7
mat rows missing none
scan_only_list_but_materializing q18 q39
measured_materializing q9(2 cols) q10(2 cols) q11(2 cols) q12(3 cols) q13(1 cols) q14(2 cols) q15(1 cols) q18(2 cols) q19(1 cols) q21(1 cols) q22(3 cols) q23(4 cols) q24(100 cols) q25(100 cols) q26(100 cols) q27(100 cols) q28(1 cols) q29(1 cols) q30(1 cols) q31(5 cols) q32(4 cols) q37(4 cols) q38(4 cols) q39(5 cols) q40(8 cols) q41(4 cols) q42(4 cols) q43(5 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q6(measured scan, list other) q18(measured materializing, list scan) q20(measured scan, list other) q36(measured scan, list other) q39(measured materializing, list scan)
worst5 warm [(1.422, 'q8'), (1.296, 'q5'), (1.275, 'q6'), (1.175, 'q7'), (1.051, 'q33')]
worst5 cold [(2.136, 'q39'), (1.73, 'q1'), (1.522, 'q7'), (1.218, 'q4'), (1.106, 'q6')]
worst5 first [(2.155, 'q39'), (1.96, 'q18'), (1.731, 'q7'), (1.247, 'q4'), (1.157, 'q8')]
worst5 materializing_warm [(1.669, 'q42'), (1.052, 'q10'), (1.049, 'q25'), (1.03, 'q15'), (1.016, 'q19')]
first_run_total_ratio 1.816 (PI after P1: 2.99)
pi_row q41 warm 0.975 first 2.026 3run 1.530
pi_row q42 warm 1.669 first 2.566 3run 2.117
```

#### arm B: parted p0 vs p2 (ms; ratio = p2/p0)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.046 | 0.01 | 0.01 | 0.557 | 0.01 | 0.01 | 0.577 | 0.01 | 0.01 | 0.612 | [] | same |
| q2 | 6.59 | 6.69 | 1.015 | 17.00 | 16.51 | 0.971 | 30.70 | 30.22 | 0.984 | 46.54 | 16.19 | 0.348 | [] | same |
| q3 | 559.60 | 5.85 | 0.010 | 570.20 | 10.46 | 0.018 | 1688.32 | 22.34 | 0.013 | 579.80 | 15.03 | 0.026 | [] | same |
| q4 | 278.42 | 7.43 | 0.027 | 327.18 | 12.41 | 0.038 | 886.53 | 27.47 | 0.031 | 366.33 | 32.92 | 0.090 | [] | same |
| q5 | 278.19 | 0.00 | 0.000 | 321.37 | 0.03 | 0.000 | 882.25 | 0.03 | 0.000 | 368.48 | 0.03 | 0.000 | [] | same |
| q6 | 348.20 | 346.09 | 0.994 | 9629.99 | 9680.01 | 1.005 | 10322.71 | 10381.83 | 1.006 | 9719.79 | 9703.62 | 0.998 | [SearchPhrase] | same |
| q7 | 524.75 | 2.54 | 0.005 | 551.08 | 7.15 | 0.013 | 1621.75 | 12.38 | 0.008 | 579.96 | 7.18 | 0.012 | [] | same |
| q8 | 6.36 | 6.60 | 1.038 | 7.95 | 8.47 | 1.065 | 21.25 | 21.85 | 1.028 | 40.98 | 8.13 | 0.198 | [] | same |
| q9 | 85.32 | 82.99 | 0.973 | 152.39 | 165.67 | 1.087 | 324.30 | 334.45 | 1.031 | 396.40 | 190.15 | 0.480 | [RegionID UserID] | same |
| q10 | 78.87 | 80.44 | 1.020 | 153.43 | 177.51 | 1.157 | 314.92 | 341.65 | 1.085 | 452.06 | 208.39 | 0.461 | [RegionID UserID] | same |
| q11 | 24.59 | 23.26 | 0.946 | 59.64 | 66.98 | 1.123 | 109.30 | 114.49 | 1.048 | 396.02 | 87.32 | 0.220 | [UserID MobilePhoneModel] | same |
| q12 | 26.78 | 25.93 | 0.968 | 61.89 | 72.54 | 1.172 | 115.62 | 124.71 | 1.079 | 451.66 | 90.84 | 0.201 | [UserID MobilePhone MobilePhoneModel] | same |
| q13 | 40.31 | 39.79 | 0.987 | 72.27 | 75.83 | 1.049 | 153.47 | 157.25 | 1.025 | 155.92 | 92.98 | 0.596 | [SearchPhrase] | same |
| q14 | 101.44 | 101.48 | 1.000 | 155.23 | 170.28 | 1.097 | 362.84 | 379.75 | 1.047 | 448.10 | 203.88 | 0.455 | [UserID SearchPhrase] | same |
| q15 | 46.64 | 46.19 | 0.990 | 89.92 | 86.90 | 0.966 | 186.75 | 181.16 | 0.970 | 210.85 | 108.01 | 0.512 | [SearchPhrase] | same |
| q16 | 72.40 | 64.32 | 0.888 | 99.09 | 88.64 | 0.895 | 246.75 | 218.06 | 0.884 | 247.10 | 153.02 | 0.619 | [] | same |
| q17 | 95.91 | 93.16 | 0.971 | 134.02 | 127.05 | 0.948 | 326.26 | 314.97 | 0.965 | 460.21 | 243.60 | 0.529 | [] | same |
| q18 | 17.08 | 17.54 | 1.027 | 85.35 | 31.02 | 0.363 | 119.92 | 67.30 | 0.561 | 340.63 | 62.46 | 0.183 | [] | same |
| q19 | 141.90 | 144.65 | 1.019 | 340.37 | 245.69 | 0.722 | 630.90 | 537.47 | 0.852 | 756.81 | 374.25 | 0.495 | [EventTime] | same |
| q20 | 4.62 | 1.57 | 0.340 | 14.55 | 12.13 | 0.834 | 24.14 | 15.75 | 0.652 | 156.51 | 23.97 | 0.153 | [] | same |
| q21 | 41.83 | 41.82 | 1.000 | 46.92 | 57.12 | 1.217 | 131.86 | 143.22 | 1.086 | 87.51 | 99.66 | 1.139 | [URL] | same |
| q22 | 60.75 | 60.26 | 0.992 | 87.52 | 110.41 | 1.261 | 210.44 | 236.75 | 1.125 | 231.93 | 186.87 | 0.806 | [UserID URL SearchPhrase] | same |
| q23 | 90.56 | 90.05 | 0.994 | 118.41 | 150.42 | 1.270 | 300.24 | 336.41 | 1.120 | 476.54 | 272.73 | 0.572 | [Title UserID URL SearchPhrase] | same |
| q24 | 44.70 | 43.64 | 0.976 | 139.66 | 609.03 | 4.361 | 229.78 | 701.83 | 3.054 | 9299.80 | 1743.42 | 0.187 | <105 columns> | same |
| q25 | 1053.20 | 1045.10 | 0.992 | 1332.70 | 1693.05 | 1.270 | 3470.57 | 3809.47 | 1.098 | 9701.03 | 2850.10 | 0.294 | <105 columns> | same |
| q26 | 1104.94 | 1102.02 | 0.997 | 1475.42 | 1838.54 | 1.246 | 3701.58 | 4043.36 | 1.092 | 9846.98 | 2967.04 | 0.301 | <105 columns> | same |
| q27 | 1066.46 | 1057.73 | 0.992 | 1343.87 | 1695.61 | 1.262 | 3491.20 | 3822.35 | 1.095 | 9679.78 | 2831.98 | 0.293 | <105 columns> | same |
| q28 | 90.59 | 87.55 | 0.966 | 149.07 | 148.84 | 0.998 | 332.35 | 324.90 | 0.978 | 311.22 | 193.15 | 0.621 | [URL] | same |
| q29 | 355.79 | 356.29 | 1.001 | 471.44 | 472.34 | 1.002 | 1192.00 | 1195.56 | 1.003 | 553.17 | 514.18 | 0.930 | [Referer] | same |
| q30 | 277.62 | 276.13 | 0.995 | 287.03 | 293.39 | 1.022 | 863.06 | 860.62 | 0.997 | 297.33 | 295.00 | 0.992 | [ResolutionWidth] | same |
| q31 | 55.55 | 56.22 | 1.012 | 125.43 | 104.47 | 0.833 | 238.44 | 219.23 | 0.919 | 424.00 | 144.03 | 0.340 | [SearchPhrase] | same |
| q32 | 68.79 | 71.43 | 1.038 | 178.48 | 131.95 | 0.739 | 319.15 | 279.40 | 0.875 | 619.07 | 304.07 | 0.491 | [SearchPhrase] | same |
| q33 | 210.94 | 217.37 | 1.030 | 374.35 | 387.62 | 1.035 | 800.98 | 824.68 | 1.030 | 611.00 | 634.25 | 1.038 | [] | same |
| q34 | 43.29 | 43.57 | 1.006 | 96.17 | 75.39 | 0.784 | 183.16 | 163.55 | 0.893 | 183.36 | 119.91 | 0.654 | [] | same |
| q35 | 42.85 | 43.37 | 1.012 | 95.34 | 75.22 | 0.789 | 181.99 | 162.63 | 0.894 | 183.71 | 118.57 | 0.645 | [] | same |
| q36 | 145.78 | 144.73 | 0.993 | 214.28 | 199.54 | 0.931 | 511.12 | 494.71 | 0.968 | 303.01 | 208.74 | 0.689 | [] | same |
| q37 | 35.80 | 34.42 | 0.962 | 69.87 | 83.49 | 1.195 | 142.52 | 153.89 | 1.080 | 233.91 | 130.21 | 0.557 | [EventDate URL IsRefresh DontCountHits] | same |
| q38 | 34.68 | 33.22 | 0.958 | 67.96 | 84.22 | 1.239 | 143.92 | 153.88 | 1.069 | 222.92 | 126.51 | 0.568 | [Title EventDate IsRefresh DontCountHits] | same |
| q39 | 15.02 | 13.24 | 0.882 | 27.64 | 40.54 | 1.467 | 57.80 | 72.16 | 1.248 | 115.86 | 49.59 | 0.428 | [EventDate URL IsRefresh IsLink IsDownload] | same |
| q40 | 116.87 | 115.19 | 0.986 | 418.51 | 360.78 | 0.862 | 659.41 | 594.21 | 0.901 | 788.68 | 460.09 | 0.583 | [EventDate IsRefresh] | same |
| q41 | 17.64 | 17.17 | 0.973 | 39.44 | 59.16 | 1.500 | 75.11 | 98.49 | 1.311 | 256.05 | 236.74 | 0.925 | [EventDate CounterID IsRefresh TraficSourceID URLHash] | same |
| q42 | 13.12 | 16.36 | 1.246 | 23.92 | 45.45 | 1.900 | 50.78 | 81.80 | 1.611 | 191.69 | 171.50 | 0.895 | [EventDate CounterID IsRefresh WindowClientWidth WindowClientHeight DontCountHits] | same |
| q43 | 28.23 | 26.56 | 0.941 | 40.51 | 63.07 | 1.557 | 97.49 | 121.55 | 1.247 | 126.94 | 159.30 | 1.255 | [EventTime EventDate IsRefresh DontCountHits] | same |

```
totals over all 43: warm 7753.0 vs 6090.0 (0.786); first run 20066.9 vs 19834.9 (0.988); 3-run 35753.6 vs 32177.7 (0.900)
answer_mismatches none
order_differences none
scan_only(gated) n=16; gmeans over base>=1ms (per column): warm n=15 0.156, first n=15 0.223, cold n=15 0.141; worst warm=1.046
below_1ms_excluded_from_warm_gmean q1
below_1ms_excluded_from_first_gmean q1
below_1ms_excluded_from_cold_gmean q1
mat rows missing none
scan_only_list_but_materializing q39
measured_materializing q6(1 cols) q9(2 cols) q10(2 cols) q11(2 cols) q12(3 cols) q13(1 cols) q14(2 cols) q15(1 cols) q19(1 cols) q21(1 cols) q22(3 cols) q23(4 cols) q24(105 cols) q25(105 cols) q26(105 cols) q27(105 cols) q28(1 cols) q29(1 cols) q30(1 cols) q31(1 cols) q32(1 cols) q37(4 cols) q38(4 cols) q39(5 cols) q40(2 cols) q41(5 cols) q42(6 cols) q43(4 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q20(measured scan, list other) q36(measured scan, list other) q39(measured materializing, list scan)
worst5 warm [(1.046, 'q1'), (1.038, 'q8'), (1.03, 'q33'), (1.027, 'q18'), (1.015, 'q2')]
worst5 cold [(1.038, 'q33'), (0.689, 'q36'), (0.654, 'q34'), (0.645, 'q35'), (0.619, 'q16')]
worst5 first [(1.467, 'q39'), (1.065, 'q8'), (1.035, 'q33'), (0.971, 'q2'), (0.948, 'q17')]
worst5 materializing_warm [(1.246, 'q42'), (1.038, 'q32'), (1.02, 'q10'), (1.019, 'q19'), (1.012, 'q31')]
pi_row q41 warm 0.973 first 1.500 3run 1.311
pi_row q42 warm 1.246 first 1.900 3run 1.611
```

#### arm C0: splayed g0, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.022 | 0.01 | 0.01 | 1.078 | 0.01 | 0.01 | 1.073 | nan | nan | nan |  | same |
| q2 | 1.11 | 0.64 | 0.576 | 1.98 | 1.64 | 0.827 | 4.29 | 3.16 | 0.738 | nan | nan | nan |  | same |
| q3 | 3.78 | 3.78 | 1.002 | 5.78 | 5.89 | 1.018 | 13.40 | 13.49 | 1.006 | nan | nan | nan |  | same |
| q4 | 0.00 | 0.00 | 0.978 | 0.02 | 0.03 | 1.305 | 0.02 | 0.03 | 1.292 | nan | nan | nan |  | same |
| q5 | 0.00 | 0.00 | 1.045 | 0.02 | 0.03 | 1.145 | 0.02 | 0.03 | 1.134 | nan | nan | nan |  | same |
| q6 | 0.00 | 0.00 | 1.053 | 0.02 | 0.03 | 1.119 | 0.02 | 0.03 | 1.110 | nan | nan | nan |  | same |
| q7 | 0.00 | 0.00 | 1.040 | 0.02 | 0.02 | 1.072 | 0.02 | 0.02 | 1.069 | nan | nan | nan |  | same |
| q8 | 3.49 | 3.50 | 1.001 | 9.00 | 8.93 | 0.992 | 16.47 | 16.08 | 0.976 | nan | nan | nan |  | same |
| q9 | 75.97 | 75.26 | 0.991 | 113.72 | 117.31 | 1.032 | 265.70 | 271.08 | 1.020 | nan | nan | nan |  | same |
| q10 | 68.90 | 68.84 | 0.999 | 96.87 | 97.47 | 1.006 | 236.47 | 236.37 | 1.000 | nan | nan | nan |  | same |
| q11 | 16.03 | 16.16 | 1.008 | 25.10 | 25.25 | 1.006 | 57.63 | 57.91 | 1.005 | nan | nan | nan |  | same |
| q12 | 16.33 | 16.21 | 0.993 | 26.10 | 25.96 | 0.995 | 59.49 | 58.95 | 0.991 | nan | nan | nan |  | same |
| q13 | 31.98 | 32.26 | 1.009 | 41.45 | 40.95 | 0.988 | 105.55 | 106.79 | 1.012 | nan | nan | nan |  | same |
| q14 | 62.93 | 62.72 | 0.997 | 88.30 | 88.48 | 1.002 | 218.23 | 218.48 | 1.001 | nan | nan | nan |  | same |
| q15 | 36.48 | 36.43 | 0.999 | 47.67 | 47.91 | 1.005 | 121.97 | 124.56 | 1.021 | nan | nan | nan |  | same |
| q16 | 69.54 | 69.80 | 1.004 | 93.71 | 95.79 | 1.022 | 234.97 | 235.59 | 1.003 | nan | nan | nan |  | same |
| q17 | 92.90 | 93.24 | 1.004 | 129.59 | 129.11 | 0.996 | 316.84 | 317.82 | 1.003 | nan | nan | nan |  | same |
| q18 | 6.94 | 6.89 | 0.993 | 14.45 | 14.60 | 1.010 | 28.55 | 28.42 | 0.995 | nan | nan | nan |  | same |
| q19 | 137.42 | 137.42 | 1.000 | 208.68 | 209.20 | 1.003 | 490.74 | 489.78 | 0.998 | nan | nan | nan |  | same |
| q20 | 1.13 | 1.17 | 1.032 | 6.83 | 6.97 | 1.021 | 9.40 | 9.66 | 1.028 | nan | nan | nan |  | same |
| q21 | 42.10 | 41.93 | 0.996 | 45.51 | 46.13 | 1.014 | 130.45 | 131.11 | 1.005 | nan | nan | nan |  | same |
| q22 | 27.52 | 27.51 | 1.000 | 32.37 | 32.09 | 0.991 | 88.06 | 88.01 | 0.999 | nan | nan | nan |  | same |
| q23 | 48.85 | 48.95 | 1.002 | 60.43 | 60.82 | 1.006 | 158.53 | 159.74 | 1.008 | nan | nan | nan |  | same |
| q24 | 29.11 | 29.35 | 1.008 | 34.46 | 34.99 | 1.015 | 93.23 | 94.08 | 1.009 | nan | nan | nan |  | same |
| q25 | 0.90 | 1.00 | 1.115 | 4.50 | 4.58 | 1.017 | 6.43 | 6.64 | 1.033 | nan | nan | nan |  | same |
| q26 | 10.75 | 10.89 | 1.013 | 15.76 | 15.90 | 1.009 | 37.37 | 37.93 | 1.015 | nan | nan | nan |  | same |
| q27 | 0.92 | 0.89 | 0.974 | 4.46 | 4.61 | 1.034 | 6.65 | 6.68 | 1.005 | nan | nan | nan |  | same |
| q28 | 82.56 | 81.18 | 0.983 | 85.31 | 83.73 | 0.981 | 253.18 | 246.76 | 0.975 | nan | nan | nan |  | same |
| q29 | 353.98 | 348.62 | 0.985 | 441.91 | 437.86 | 0.991 | 1155.87 | 1139.93 | 0.986 | nan | nan | nan |  | same |
| q30 | 0.02 | 0.02 | 1.014 | 3.00 | 3.00 | 0.999 | 3.06 | 3.06 | 1.000 | nan | nan | nan |  | same |
| q31 | 52.96 | 51.18 | 0.966 | 70.75 | 70.18 | 0.992 | 177.13 | 173.56 | 0.980 | nan | nan | nan |  | same |
| q32 | 59.38 | 58.96 | 0.993 | 86.45 | 85.40 | 0.988 | 205.69 | 204.36 | 0.994 | nan | nan | nan |  | same |
| q33 | 202.48 | 203.58 | 1.005 | 364.64 | 364.41 | 0.999 | 777.62 | 774.65 | 0.996 | nan | nan | nan |  | same |
| q34 | 39.31 | 39.09 | 0.994 | 70.63 | 69.87 | 0.989 | 150.44 | 148.73 | 0.989 | nan | nan | nan |  | same |
| q35 | 39.27 | 39.27 | 1.000 | 70.63 | 69.75 | 0.988 | 149.92 | 149.48 | 0.997 | nan | nan | nan |  | same |
| q36 | 64.14 | 64.00 | 0.998 | 88.21 | 88.71 | 1.006 | 217.23 | 217.43 | 1.001 | nan | nan | nan |  | same |
| q37 | 17.32 | 16.65 | 0.962 | 27.82 | 27.16 | 0.976 | 62.53 | 60.60 | 0.969 | nan | nan | nan |  | same |
| q38 | 15.85 | 14.93 | 0.942 | 25.28 | 24.39 | 0.964 | 57.90 | 55.03 | 0.950 | nan | nan | nan |  | same |
| q39 | 7.47 | 7.09 | 0.950 | 14.91 | 14.65 | 0.982 | 30.23 | 29.10 | 0.962 | nan | nan | nan |  | same |
| q40 | 58.48 | 58.21 | 0.995 | 90.96 | 90.06 | 0.990 | 211.28 | 207.94 | 0.984 | nan | nan | nan |  | same |
| q41 | 5.82 | 5.73 | 0.985 | 13.42 | 13.32 | 0.993 | 25.51 | 24.89 | 0.976 | nan | nan | nan |  | same |
| q42 | 4.56 | 4.47 | 0.980 | 11.08 | 11.02 | 0.995 | 20.55 | 20.37 | 0.991 | nan | nan | nan |  | same |
| q43 | 9.29 | 8.98 | 0.967 | 18.99 | 18.34 | 0.966 | 38.28 | 37.42 | 0.977 | nan | nan | nan |  | same |

```
totals over all 43: warm 1798.0 vs 1786.8 (0.994); first run 2590.8 vs 2586.5 (0.998); 3-run 6236.9 vs 6205.7 (0.995)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=35)=0.977 warm_worst=1.115 worst5 [(1.115, 'q25'), (1.053, 'q6'), (1.045, 'q5'), (1.04, 'q7'), (1.032, 'q20')]
```

#### arm C1: parted p0, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.108 | 0.01 | 0.01 | 0.860 | 0.01 | 0.01 | 0.876 | nan | nan | nan |  | same |
| q2 | 6.61 | 6.62 | 1.001 | 17.10 | 16.70 | 0.976 | 30.59 | 30.41 | 0.994 | nan | nan | nan |  | same |
| q3 | 536.96 | 558.73 | 1.041 | 558.24 | 568.39 | 1.018 | 1637.52 | 1696.56 | 1.036 | nan | nan | nan |  | same |
| q4 | 278.85 | 276.42 | 0.991 | 327.74 | 324.35 | 0.990 | 888.40 | 891.36 | 1.003 | nan | nan | nan |  | same |
| q5 | 275.37 | 281.03 | 1.021 | 328.29 | 326.72 | 0.995 | 884.00 | 895.09 | 1.013 | nan | nan | nan |  | same |
| q6 | 343.11 | 348.16 | 1.015 | 9698.42 | 9731.45 | 1.003 | 10403.65 | 10443.97 | 1.004 | nan | nan | nan |  | same |
| q7 | 526.58 | 529.63 | 1.006 | 554.73 | 558.38 | 1.007 | 1616.05 | 1621.21 | 1.003 | nan | nan | nan |  | same |
| q8 | 9.13 | 6.36 | 0.696 | 21.38 | 7.98 | 0.373 | 39.98 | 20.90 | 0.523 | nan | nan | nan |  | same |
| q9 | 89.77 | 85.68 | 0.954 | 179.80 | 152.55 | 0.848 | 363.05 | 328.43 | 0.905 | nan | nan | nan |  | same |
| q10 | 87.39 | 79.39 | 0.908 | 205.13 | 158.15 | 0.771 | 381.58 | 321.47 | 0.842 | nan | nan | nan |  | same |
| q11 | 357.48 | 24.06 | 0.067 | 576.13 | 59.26 | 0.103 | 1304.50 | 110.45 | 0.085 | nan | nan | nan |  | same |
| q12 | 357.24 | 26.78 | 0.075 | 572.06 | 60.89 | 0.106 | 1299.07 | 115.33 | 0.089 | nan | nan | nan |  | same |
| q13 | 40.19 | 40.25 | 1.002 | 70.56 | 72.85 | 1.032 | 151.58 | 154.31 | 1.018 | nan | nan | nan |  | same |
| q14 | 1133.32 | 102.95 | 0.091 | 1403.29 | 153.26 | 0.109 | 3678.46 | 366.48 | 0.100 | nan | nan | nan |  | same |
| q15 | 46.67 | 46.53 | 0.997 | 89.81 | 90.20 | 1.004 | 185.20 | 185.52 | 1.002 | nan | nan | nan |  | same |
| q16 | 76.44 | 72.57 | 0.949 | 145.56 | 98.79 | 0.679 | 302.96 | 244.82 | 0.808 | nan | nan | nan |  | same |
| q17 | 103.63 | 95.54 | 0.922 | 204.53 | 133.73 | 0.654 | 413.35 | 326.89 | 0.791 | nan | nan | nan |  | same |
| q18 | 17.21 | 17.09 | 0.993 | 84.12 | 83.53 | 0.993 | 119.39 | 117.59 | 0.985 | nan | nan | nan |  | same |
| q19 | 141.87 | 141.45 | 0.997 | 334.42 | 335.92 | 1.004 | 622.65 | 622.25 | 0.999 | nan | nan | nan |  | same |
| q20 | 4.61 | 4.68 | 1.014 | 14.34 | 14.44 | 1.007 | 23.89 | 24.02 | 1.005 | nan | nan | nan |  | same |
| q21 | 41.90 | 41.74 | 0.996 | 48.45 | 46.94 | 0.969 | 136.53 | 132.31 | 0.969 | nan | nan | nan |  | same |
| q22 | 60.68 | 60.77 | 1.001 | 85.82 | 86.91 | 1.013 | 209.57 | 208.71 | 0.996 | nan | nan | nan |  | same |
| q23 | 93.79 | 90.61 | 0.966 | 206.67 | 117.78 | 0.570 | 395.79 | 299.80 | 0.757 | nan | nan | nan |  | same |
| q24 | 44.48 | 44.41 | 0.998 | 127.02 | 127.19 | 1.001 | 215.96 | 217.93 | 1.009 | nan | nan | nan |  | same |
| q25 | 1074.07 | 1052.47 | 0.980 | 1331.71 | 1328.17 | 0.997 | 3481.83 | 3441.38 | 0.988 | nan | nan | nan |  | same |
| q26 | 1125.78 | 1106.85 | 0.983 | 1473.13 | 1466.61 | 0.996 | 3745.40 | 3705.24 | 0.989 | nan | nan | nan |  | same |
| q27 | 1084.67 | 1065.58 | 0.982 | 1338.58 | 1326.23 | 0.991 | 3515.11 | 3478.92 | 0.990 | nan | nan | nan |  | same |
| q28 | 92.14 | 91.33 | 0.991 | 147.79 | 147.94 | 1.001 | 336.72 | 331.77 | 0.985 | nan | nan | nan |  | same |
| q29 | 362.16 | 357.81 | 0.988 | 471.83 | 469.18 | 0.994 | 1204.29 | 1189.85 | 0.988 | nan | nan | nan |  | same |
| q30 | 267.68 | 277.81 | 1.038 | 284.43 | 286.33 | 1.007 | 830.87 | 857.70 | 1.032 | nan | nan | nan |  | same |
| q31 | 55.42 | 55.62 | 1.004 | 124.75 | 124.97 | 1.002 | 237.74 | 239.72 | 1.008 | nan | nan | nan |  | same |
| q32 | 68.90 | 69.00 | 1.001 | 179.08 | 178.04 | 0.994 | 320.00 | 321.12 | 1.004 | nan | nan | nan |  | same |
| q33 | 217.16 | 211.97 | 0.976 | 468.10 | 372.87 | 0.797 | 909.83 | 805.93 | 0.886 | nan | nan | nan |  | same |
| q34 | 43.57 | 43.12 | 0.990 | 96.72 | 95.36 | 0.986 | 184.03 | 182.53 | 0.992 | nan | nan | nan |  | same |
| q35 | 390.54 | 43.40 | 0.111 | 2301.47 | 96.71 | 0.042 | 3083.73 | 184.33 | 0.060 | nan | nan | nan |  | same |
| q36 | 147.42 | 145.49 | 0.987 | 216.52 | 216.25 | 0.999 | 520.36 | 514.19 | 0.988 | nan | nan | nan |  | same |
| q37 | 36.76 | 36.10 | 0.982 | 69.49 | 69.32 | 0.998 | 144.55 | 142.21 | 0.984 | nan | nan | nan |  | same |
| q38 | 35.90 | 35.26 | 0.982 | 68.57 | 67.62 | 0.986 | 146.53 | 143.13 | 0.977 | nan | nan | nan |  | same |
| q39 | 15.41 | 15.13 | 0.982 | 27.72 | 27.41 | 0.989 | 58.55 | 57.99 | 0.990 | nan | nan | nan |  | same |
| q40 | 117.80 | 117.17 | 0.995 | 419.75 | 421.61 | 1.004 | 658.80 | 660.14 | 1.002 | nan | nan | nan |  | same |
| q41 | 17.73 | 17.63 | 0.994 | 38.83 | 38.89 | 1.002 | 75.22 | 74.57 | 0.991 | nan | nan | nan |  | same |
| q42 | 13.44 | 13.12 | 0.977 | 24.02 | 23.87 | 0.994 | 51.51 | 51.09 | 0.992 | nan | nan | nan |  | same |
| q43 | 28.72 | 28.28 | 0.985 | 41.00 | 41.17 | 1.004 | 99.03 | 98.66 | 0.996 | nan | nan | nan |  | same |

```
totals over all 43: warm 9868.6 vs 7764.6 (0.787); first run 24977.1 vs 20124.9 (0.806); 3-run 44907.8 vs 35856.3 (0.798)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=42)=0.776 warm_worst=1.108 worst5 [(1.108, 'q1'), (1.041, 'q3'), (1.038, 'q30'), (1.021, 'q5'), (1.015, 'q6')]
q16 first run base 145.6 ms, p2 98.8 ms (carry 2)
```

#### arm R: parted ragged, base vs p2 binary (ms; ratio = p2/base)

| q | warm_a | warm_b | wr | first_a | first_b | fr | 3run_a | 3run_b | tr | cold_a | cold_b | cr | materialized | answers |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| q1 | 0.00 | 0.00 | 1.009 | 0.01 | 0.01 | 1.144 | 0.01 | 0.01 | 1.136 | nan | nan | nan |  | same |
| q2 | 7.76 | 7.56 | 0.975 | 18.19 | 17.16 | 0.944 | 33.77 | 32.27 | 0.956 | nan | nan | nan |  | same |
| q3 | 578.79 | 282.82 | 0.489 | 592.24 | 293.67 | 0.496 | 1755.26 | 860.58 | 0.490 | nan | nan | nan |  | same |
| q4 | 275.04 | 275.99 | 1.003 | 323.09 | 322.98 | 1.000 | 875.03 | 882.94 | 1.009 | nan | nan | nan |  | same |
| q5 | 273.95 | 278.98 | 1.018 | 328.18 | 324.35 | 0.988 | 882.66 | 884.92 | 1.003 | nan | nan | nan |  | same |
| q6 | 342.84 | 346.64 | 1.011 | 9674.16 | 9722.34 | 1.005 | 10366.30 | 10413.12 | 1.005 | nan | nan | nan |  | same |
| q7 | 524.12 | 525.43 | 1.003 | 555.39 | 554.24 | 0.998 | 1620.54 | 1608.67 | 0.993 | nan | nan | nan |  | same |
| q8 | 9.61 | 8.75 | 0.911 | 22.56 | 10.28 | 0.456 | 41.90 | 27.87 | 0.665 | nan | nan | nan |  | same |
| q9 | 89.12 | 85.58 | 0.960 | 180.49 | 152.64 | 0.846 | 359.73 | 323.84 | 0.900 | nan | nan | nan |  | same |
| q10 | 87.41 | 79.47 | 0.909 | 203.34 | 153.84 | 0.757 | 380.81 | 316.04 | 0.830 | nan | nan | nan |  | same |
| q11 | 373.77 | 23.68 | 0.063 | 644.06 | 57.46 | 0.089 | 1406.18 | 106.65 | 0.076 | nan | nan | nan |  | same |
| q12 | 375.31 | 26.49 | 0.071 | 644.49 | 60.79 | 0.094 | 1399.42 | 114.96 | 0.082 | nan | nan | nan |  | same |
| q13 | 39.86 | 39.96 | 1.003 | 69.53 | 70.83 | 1.019 | 150.09 | 152.82 | 1.018 | nan | nan | nan |  | same |
| q14 | 1178.45 | 101.52 | 0.086 | 1501.43 | 153.94 | 0.103 | 3876.60 | 363.22 | 0.094 | nan | nan | nan |  | same |
| q15 | 46.60 | 46.34 | 0.995 | 89.81 | 89.35 | 0.995 | 187.11 | 186.23 | 0.995 | nan | nan | nan |  | same |
| q16 | 75.97 | 72.53 | 0.955 | 146.94 | 98.26 | 0.669 | 300.06 | 243.94 | 0.813 | nan | nan | nan |  | same |
| q17 | 103.16 | 95.46 | 0.925 | 203.22 | 134.24 | 0.661 | 411.42 | 329.21 | 0.800 | nan | nan | nan |  | same |
| q18 | 17.16 | 17.11 | 0.997 | 84.07 | 83.62 | 0.995 | 119.17 | 117.98 | 0.990 | nan | nan | nan |  | same |
| q19 | 141.28 | 141.09 | 0.999 | 338.16 | 335.93 | 0.993 | 624.46 | 621.36 | 0.995 | nan | nan | nan |  | same |
| q20 | 4.49 | 4.44 | 0.990 | 13.87 | 14.29 | 1.031 | 23.34 | 23.49 | 1.006 | nan | nan | nan |  | same |
| q21 | 41.65 | 41.77 | 1.003 | 46.64 | 46.65 | 1.000 | 130.20 | 131.21 | 1.008 | nan | nan | nan |  | same |
| q22 | 60.44 | 60.49 | 1.001 | 85.40 | 86.72 | 1.016 | 206.67 | 207.61 | 1.005 | nan | nan | nan |  | same |
| q23 | 93.82 | 90.04 | 0.960 | 266.72 | 118.30 | 0.444 | 457.63 | 299.99 | 0.656 | nan | nan | nan |  | same |
| q24 | 44.34 | 44.55 | 1.005 | 191.47 | 168.66 | 0.881 | 284.79 | 262.16 | 0.921 | nan | nan | nan |  | same |
| q25 | 1119.36 | 1103.61 | 0.986 | 1422.38 | 1388.75 | 0.976 | 3677.66 | 3607.33 | 0.981 | nan | nan | nan |  | same |
| q26 | 1178.30 | 1162.15 | 0.986 | 1575.60 | 1540.52 | 0.978 | 3943.13 | 3875.83 | 0.983 | nan | nan | nan |  | same |
| q27 | 1127.49 | 1113.84 | 0.988 | 1441.46 | 1400.35 | 0.971 | 3718.50 | 3641.63 | 0.979 | nan | nan | nan |  | same |
| q28 | 91.69 | 91.02 | 0.993 | 148.12 | 149.64 | 1.010 | 333.52 | 334.54 | 1.003 | nan | nan | nan |  | same |
| q29 | 360.94 | 356.33 | 0.987 | 470.32 | 467.26 | 0.994 | 1205.90 | 1186.74 | 0.984 | nan | nan | nan |  | same |
| q30 | 265.00 | 277.49 | 1.047 | 283.79 | 286.02 | 1.008 | 830.28 | 855.86 | 1.031 | nan | nan | nan |  | same |
| q31 | 55.48 | 55.10 | 0.993 | 124.64 | 123.71 | 0.993 | 238.34 | 236.29 | 0.991 | nan | nan | nan |  | same |
| q32 | 68.71 | 69.21 | 1.007 | 176.43 | 177.95 | 1.009 | 316.97 | 316.60 | 0.999 | nan | nan | nan |  | same |
| q33 | 216.42 | 212.28 | 0.981 | 462.11 | 372.43 | 0.806 | 905.00 | 801.73 | 0.886 | nan | nan | nan |  | same |
| q34 | 43.40 | 43.00 | 0.991 | 95.99 | 95.34 | 0.993 | 183.39 | 181.61 | 0.990 | nan | nan | nan |  | same |
| q35 | 411.54 | 43.15 | 0.105 | 2397.56 | 95.23 | 0.040 | 3237.26 | 182.08 | 0.056 | nan | nan | nan |  | same |
| q36 | 148.10 | 145.22 | 0.981 | 217.59 | 214.29 | 0.985 | 520.05 | 511.30 | 0.983 | nan | nan | nan |  | same |
| q37 | 36.19 | 35.60 | 0.984 | 73.95 | 73.88 | 0.999 | 148.45 | 147.07 | 0.991 | nan | nan | nan |  | same |
| q38 | 35.64 | 34.77 | 0.976 | 73.37 | 73.76 | 1.005 | 145.99 | 144.65 | 0.991 | nan | nan | nan |  | same |
| q39 | 15.00 | 14.96 | 0.997 | 28.95 | 28.56 | 0.986 | 59.81 | 65.04 | 1.088 | nan | nan | nan |  | same |
| q40 | 151.51 | 154.37 | 1.019 | 461.13 | 451.62 | 0.979 | 771.09 | 769.30 | 0.998 | nan | nan | nan |  | same |
| q41 | 17.59 | 17.23 | 0.980 | 40.02 | 38.10 | 0.952 | 75.68 | 72.81 | 0.962 | nan | nan | nan |  | same |
| q42 | 13.26 | 12.76 | 0.962 | 26.28 | 23.79 | 0.905 | 53.05 | 49.45 | 0.932 | nan | nan | nan |  | same |
| q43 | 28.18 | 28.00 | 0.994 | 42.16 | 41.63 | 0.987 | 100.10 | 98.07 | 0.980 | nan | nan | nan |  | same |

```
totals over all 43: warm 10168.7 vs 7666.8 (0.754); first run 25785.3 vs 20113.4 (0.780); 3-run 46357.3 vs 35589.0 (0.768)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=42)=0.763 warm_worst=1.047 worst5 [(1.047, 'q30'), (1.019, 'q40'), (1.018, 'q5'), (1.011, 'q6'), (1.009, 'q1')]
```

## P3 aware paths (10M store, release build, commit 7a603266)

**The first run below (7a603266, "FAIL (pending)") is kept as history.
"Gate rerun (HEAD 2f949edc, after Task 14b)" at the end of this section
supersedes it and holds the verdict: P3 gate: PASS (Ruling 14-8).**

The P3 gate (plan: compressed-columns P3, Task 14).  The P2 driver ran
unchanged: `W=/home/hetoku/data/tmp/p3_bench p2_bench.sh all`, 07:14-08:55,
with HEAD 7a603266 (release, sha256 95039693ca59) against the base 80a28e49
(release from `git archive`, sha256 f1075b366780).  It ran all 43 queries
on arms A, B, C0, C1 and R, in 5 alternated rounds, with 3 cold trials per
query (fadvise eviction) and full answers compared; the ragged partition 9
keeps 40 columns.  The verdict is `p3_summary.py` (P2's `p2_summary.py`
unchanged, plus the P3 clauses).  Raw data:
/home/hetoku/data/tmp/p3_bench/p2.tsv, with summary.txt beside it.  Nothing
else heavy ran: the CI runner's listener was idle the whole time, no
Runner.Worker process appeared, and the load came from the driver alone.

Sizes are unchanged (g0 5 948 677 939 B, g2 3 307 128 731 B; p0
4 807 844 987 B, p2 2 072 366 907 B).  Peak RSS over all 43 queries is now
lower on generation 2 than on generation 0: g0 5 120 100 kB against g2
5 060 388 kB (x0.988; P2: x1.58), and p0 12 158 788 kB against p2
11 963 400 kB (x0.984).

### Gate lines

```
GATE (strict) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=FAIL warm=PASS q16_first=PASS] gen0-ragged[answers=FAIL warm=PASS]
P2 GATE (strict): FAIL
p3_unexpected_materializations A/q23:SearchPhrase B/q6:SearchPhrase B/q28:URL
rss A: 5120100 -> 5060388 kB (x0.988, bound 1.1)
rss B: 12158788 -> 11963400 kB (x0.984, bound 1.1)
P3GATE p2_strict=FAIL materialization=FAIL rss=PASS
P3 GATE: FAIL
```

**P3 gate (first run): FAIL (pending)**, superseded by the rerun below.
The controller ruled on each failing clause (rulings 14-1 to 14-7: fixed in
14b, or accepted).
Parted (arm B) passes every P2 clause on its own, with no exemption.

### Failing clauses

The profiles are `perf record -g` on the release binary, with the query run
5-30 times in one process; each line gives the top symbols as self samples,
g0 -> g2.

| clause | query, arm | a -> b (ms) | ratio | owning path (diagnosis) |
|---|---|---|---|---|
| splayed warm (scan-only) | q13, A | 22.30 -> 31.54 | 1.414 | SYM-key group over the staged fused radix |
| splayed first | q13, A | 32.21 -> 40.57 | 1.260 | same |
| splayed warm | q15, A | 26.83 -> 35.98 | 1.341 | same (keys SearchEngineID, SearchPhrase) |
| splayed first | q15, A | 39.58 -> 49.43 | 1.249 | same |
| splayed warm | q29, A | 347.47 -> 425.13 | 1.224 | group by Referer: an extra min/max pass and a packed agg read |
| splayed first | q29, A | 434.76 -> 515.64 | 1.186 | same |
| splayed cold | q29, A | 500.27 -> 554.97 | 1.109 | same |
| splayed warm | q8, A | 3.59 -> 4.05 | 1.128 | run-level dense count; P3_DEFERRALS exempts it, the strict line does not |
| splayed materializing warm | q22, A | 18.04 -> 20.55 | 1.139 | SearchPhrase key decoded per morsel after the URL LIKE |
| gen0-splayed warm (C0) | q27 | 0.90 -> 1.00 | 1.110 | sub-ms gate statistic, not a regression (re-check below) |
| gen0-parted / gen0-ragged answers | q40, C1 and R | - | DIFF | tie order at the `take: 1010` boundary, not a wrong answer |
| materialization | q23, A | SearchPhrase | - | count-distinct group key, outside ALLOW |
| materialization | q6, B | SearchPhrase | - | parted `(at hits 'SearchPhrase)` takes the whole column |
| materialization | q28, B | URL | - | `strlen URL` in the per-partition group |

- **q13 and q15, splayed.**  Generation 2 did not slow down.  Its time
  stayed where the P2 gate had it (q13 31.87 -> 31.54 ms, q15 37.51 ->
  35.98 ms).  Generation 0 sped up during P3 (C0: q13 0.709, q15 0.724; g0
  31.91 -> 22.30 ms) and generation 2 did not follow.  The profiles:
  - g0 (q13): agg_radix_group_fn 3 081, agg_radix_scatter_fn 2 112,
    rowsel_emit_segment 1 809, agg_radix_count_fn 1 267,
    agg_sel_cursor_next 953;
  - g2 (q13): agg_radix_group_fused_fn 3 342, agg_radix_stage_rows 1 834,
    rowsel_emit_segment 1 812, dec_fast_runs 1 382, eval_runs 1 170;
  - q15 matches, with ray_enccol_vview 869 in sixth place.

  Generation 0 groups over the selection with the plain radix.  Generation
  2 stages the SYM key rows (`agg_radix_stage_rows`) and decodes the key per
  morsel (dec_fast_runs, eval_runs and the view: about 18% of the samples).
  The owning path is the group-by key read on generation 2: the staged fused
  radix in agg_engine.c / group.c (Path 5 ids by range).
- **q22, splayed** (materializing: URL by LIKE).  The P3 gain is the same as
  for q13: g0 27.53 -> 18.04 ms, g2 27.56 -> 20.55 ms.  The profiles:
  - g0: like_rows_fn 7 865, memmem 3 573, rowsel_emit_segment 2 644,
    dom_parse_strl_atoms 803, expr_eval_morsel 826;
  - g2: like_rows_fn 7 700, memmem 3 651, rowsel_emit_segment 2 664,
    eval_runs 1 736, dom_parse_strl_atoms 826, ray_enccol_vview 609.

  The extra eval_runs and view time is the SearchPhrase key and its
  `!= ""` filter read by range.  It is the same owner as q13.
- **q29, splayed** (`by: Referer`, `avg (strlen Referer)`).  This one is a
  generation-2 slowdown since P2: g2 349.10 -> 425.13 ms, with g0
  unchanged.  P2 materialized Referer, and the group then read the flat
  column.  The profiles:
  - g0: radix_phase1_body 21 124, radix_phase3_fn 10 568,
    accum_from_entry_nullable 3 800, radix_phase2_fn 3 081,
    exec_group_run 1 412;
  - g2: radix_phase1_body 14 216, minmax_scan_body 11 631,
    radix_phase3_fn 10 456, ght_pack_aggs_piece 7 256,
    accum_from_entry_nullable 3 922.

  The DWARF call graph puts minmax_scan_body under exec_group_run ->
  minmax_scan_fn -> group_sel_walk -> ray_col_walk -> minmax_piece, and
  ght_pack_aggs_piece under radix_phase1_body.  Generation 2 makes a range
  pass over the key for its bounds, which generation 0 does not make, and it
  packs the aggregate inputs piece by piece.  The owning path is the
  exec_group_run range-read setup (min/max key bounds by a column walk
  instead of from the directory or the SYM bound), group.c.
- **q8, splayed** (watch item, was 1.17 after Task 3b).  The profiles:
  - g0: rowsel_emit_segment 1 315, dom_parse_strl_atoms 792,
    expr_eval_morsel 679, expr_load_i64 389, agg_dense_sel_rows 294;
  - g2: rowsel_emit_segment 1 252, dom_parse_strl_atoms 780, eval_runs 279,
    ray_enccol_vview 263, agg_dense_runs_sel 194.

  What remains is the run view built for the filter and again for the dense
  count (the triple-view structure ruled on at Task 3b).  P3_DEFERRALS
  exempts it (cap 2.13), but the strict line counts it.
- **Gen-0 q27 (C0)** (`asc: [EventTime SearchPhrase] take: 10`).  Both
  medians are about 1 ms, and the gap of 0.10 ms is above the 0.05 ms floor.
  The gate statistic takes the best round: base 0.902 against p2 1.001.  The
  medians of the rounds are 0.983 and 1.039.  A 9-round alternated re-check
  on g0 gave a median of 1.031 ms for both binaries, and best rounds of 0.886
  and 0.918.  This is the same sub-ms artifact as P2's gen-0 q25.  Gen-0 q30
  at 1.773 passes on the floor (0.02 -> 0.03 ms).
- **q40, gen-0 parted and ragged** (base against p2, both p0).  The whole
  grouped result (354 662 rows) is byte-identical once sorted.  945 groups
  have PageViews > 13, and 131 tie at 13.  `desc: PageViews take: 1010` cuts
  through that tie, so `take: -10` returns ten of the tied groups, and which
  ten depends on tie order.  The new binary now returns the same ten as
  arms A, B and C0 (raw hash dd41af295a7d).  The base returns a different
  ten (2da5238161f2).  The driver's sorted compare cannot tell a cut tie set
  from a wrong answer, so it reports DIFF.  At the P2 gate (df637f04) the
  p2 binary still matched the base here.
- **Materializations outside ALLOW**:
  - **A/q23 SearchPhrase**: taken directly by ray_select_impl, on the
    count-distinct route (UserID by count_distinct_per_group_buf).  This is
    the same class as q10's RegionID: the group key of a group that has a
    holistic aggregate.  Parted q23 does not materialize it.  Covering it
    would widen ALLOW, which needs a ruling (D7).
  - **B/q6 SearchPhrase**: `(count (distinct (at hits 'SearchPhrase)))`.
    `ray_table_col_value` materializes the parted column for `at`, and
    `enc_materialize_parted` fans out to every segment.  Splayed `at` is
    lazy and materializes nothing.  The class is the eval-level column value
    of a parted table (holistic distinct).  The profile is the same on p0
    and p2 (sym_intern_nolock 12 815 / 12 690, ray_str_len 2 415 / 2 409),
    and so is the time (warm 1.008).
  - **B/q28 URL**: exec_strlen inside exec_group_per_partition
    materializes each partition's URL (10 segments).  The class is a row
    expression (`strlen`) over a STR column in the per-partition group.  No
    ALLOW class covers it, and splayed q28 materializes nothing.  The
    profile is the same on p0 and p2 (strlen_sym_task 4 798 / 4 782,
    agg_dense_sel_rows 1 965 / 2 071), and so is the time (0.964).
  - The owning paths: parted `at` (eval / table column value) and the
    per-partition group's expression inputs (exec_group_per_partition).

### Materialization classes, traced

The traces ran gdb on the release binary, with breakpoints on
`enc_materialize_single` and `enc_materialize_parted` printing the column
path and the backtrace, on generation-2 stores rebuilt after the run under
the same driver paths.

| query | ALLOW entry | A (splayed) traced | B (parted) traced | class / outcome |
|---|---|---|---|---|
| q11 | UserID, MobilePhoneModel | both, try_count_distinct_v2_rewrite | none | count distinct (holistic): holds |
| q12 | UserID, MobilePhone, MobilePhoneModel | none | none | nested group: narrowed out of ALLOW |
| q14 | UserID, SearchPhrase | both, try_count_distinct_v2_rewrite | none | count distinct (holistic): holds |
| q22 | URL | URL, exec_like | URL, exec_like | LIKE: holds |
| q23 | Title, URL, UserID | Title and URL by exec_like, UserID by count_distinct_per_group_buf, plus SearchPhrase (ray_select_impl) | Title, URL by exec_like | LIKE and count distinct: hold; SearchPhrase is outside the entry (see above) |
| q24 | EventTime, URL | EventTime, ray_col_src_plain in ray_fused_topk_select | URL, exec_like | sort key (A); LIKE outside the fused predicate (B): both hold |
| q40 | URL, Referer, SearchEngineID, AdvEngineID, TraficSourceID | none | none | computed key Src: narrowed out of ALLOW |

The other materializations take the classes ALLOW states:
- q9 and q10: count distinct (try_count_distinct_v2_rewrite,
  count_distinct_per_group_buf);
- q19: `minute EventTime`, by bind_referenced_columns (A) and exec_extract
  (B);
- q21: LIKE in ray_try_count_select_expr;
- q25-q27: sort keys in ray_fused_topk_select;
- q30: `set`, by ray_lazy_materialize.

### Carries and watch list

Each carry is printed with its P2 re-run ratio and today's:

```
carry q2/A/warm: P2 x1.086 -> P3 x0.606
carry q8/A/warm: P2 x1.424 -> P3 x1.128
carry q8/B/warm: P2 x1.060 -> P3 x0.356
carry q18/A/first: P2 x1.983 -> P3 x0.487
carry q18/B/warm: P2 x1.088 -> P3 x0.764
carry q39/A/first: P2 x2.402 -> P3 x0.725
carry q39/B/first: P2 x1.503 -> P3 x0.740
carry q42/A/warm: P2 x1.664 -> P3 x1.012
carry q42/B/warm: P2 x1.237 -> P3 x1.007
```

Every carry is within its P2 clause bound except q8/A/warm (1.128 > 1.05).
The splayed rss carries (q24-q27) are x0.99 now.

| watch item | number |
|---|---|
| q8 splayed warm (was 1.17) | 1.128 (first 0.997, cold 0.243); parted q8 warm 0.356 |
| q10 | splayed warm 1.024 (materializing, passes), first 1.176, cold 0.374; parted warm 1.007 |
| q26 splayed warm (was 1.058) | 1.044 |
| parted q42 (was 1.03-1.08) | warm 1.007, first 1.006, 3-run 1.074, cold 0.769 |
| arm R, gen-0 ragged | warm gmean 0.588 (n=42), worst 1.018 (q1, below 1 ms); answers all same except q40's tie order |
| parted nullable avg (parted q4) | p0 272.86 ms, p2 0.00 ms (answered without a decode); gen 0 base -> p2 0.974 (C1) and 0.991 (R) |
| URL first-touch, q24 | splayed first 1.377 (URL is not materialized on A, only the EventTime sort key); parted first 1.220 (URL by LIKE) |
| q24-q27 first run (reported, not gated) | splayed 1.377 / 3.064 / 1.371 / 4.306; parted 1.220 / 0.770 / 0.964 / 0.740 |
| SYM `in` later in a chain (ledger line 88) | still materializes, on splayed generation 2: `(and (== IsRefresh 0) (in MobilePhoneModel ['iPad 'iPhone]))` leaves MobilePhoneModel materialized (generic OP_IN in exec_node_inner); the integer `(in TraficSourceID [-1 6])` after `(== IsRefresh 0)` does the same, so the shape is wider than SYM; `in` first in the chain: 0 |
| parted `in` (Task 12) | 0 materializations: integer and SYM, first and later in the chain, also under a group |

### Compared with the final P2 gate (same store, same driver)

The ratios are generation-2 warm, P3 / P2; generation-0 times moved too,
so the gen2/gen0 ratio for each run is shown beside it.

| q | A g2 warm P2 -> P3 (ms) | A P3/P2 | A g2/g0 P2 -> P3 | B p2 warm P2 -> P3 (ms) | B P3/P2 | B p2/p0 P2 -> P3 |
|---|---|---|---|---|---|---|
| q1 | 0.00 -> 0.00 | - | 1.000 -> 1.000 | 0.00 -> 0.00 | - | 1.046 -> 1.052 |
| q2 | 0.65 -> 0.39 | 0.600 | 1.049 -> 0.606 | 6.69 -> 6.43 | 0.961 | 1.015 -> 0.953 |
| q3 | 0.00 -> 0.00 | - | 0.001 -> 0.001 | 5.85 -> 0.00 | 0.000 | 0.010 -> 0.000 |
| q4 | 0.00 -> 0.00 | - | 1.013 -> 1.068 | 7.43 -> 0.00 | 0.000 | 0.027 -> 0.000 |
| q5 | 0.00 -> 0.00 | - | 1.296 -> 1.314 | 0.00 -> 0.00 | - | 0.000 -> 0.000 |
| q6 | 0.00 -> 0.00 | - | 1.275 -> 1.244 | 346.09 -> 342.61 | 0.990 | 0.994 -> 1.008 |
| q7 | 0.00 -> 0.00 | - | 1.175 -> 1.143 | 2.54 -> 0.00 | 0.000 | 0.005 -> 0.000 |
| q8 | 5.06 -> 4.05 | 0.800 | 1.422 -> 1.128 | 6.60 -> 2.25 | 0.341 | 1.038 -> 0.356 |
| q9 | 74.96 -> 75.25 | 1.004 | 0.999 -> 0.997 | 82.99 -> 83.23 | 1.003 | 0.973 -> 0.974 |
| q10 | 72.61 -> 71.13 | 0.980 | 1.052 -> 1.024 | 80.44 -> 79.37 | 0.987 | 1.020 -> 1.007 |
| q11 | 16.18 -> 11.70 | 0.723 | 1.016 -> 1.004 | 23.26 -> 16.14 | 0.694 | 0.946 -> 0.790 |
| q12 | 16.38 -> 11.68 | 0.713 | 1.010 -> 0.988 | 25.93 -> 17.95 | 0.692 | 0.968 -> 0.780 |
| q13 | 31.87 -> 31.54 | 0.990 | 0.999 -> 1.414 | 39.79 -> 37.92 | 0.953 | 0.987 -> 1.025 |
| q14 | 62.68 -> 53.37 | 0.851 | 0.999 -> 1.014 | 101.48 -> 85.77 | 0.845 | 1.000 -> 0.867 |
| q15 | 37.51 -> 35.98 | 0.959 | 1.030 -> 1.341 | 46.19 -> 44.84 | 0.971 | 0.990 -> 1.035 |
| q16 | 63.35 -> 60.88 | 0.961 | 0.907 -> 0.874 | 64.32 -> 61.44 | 0.955 | 0.888 -> 0.848 |
| q17 | 92.67 -> 90.22 | 0.974 | 0.992 -> 0.953 | 93.16 -> 90.11 | 0.967 | 0.971 -> 0.943 |
| q18 | 6.87 -> 5.01 | 0.729 | 1.004 -> 0.736 | 17.54 -> 13.18 | 0.751 | 1.027 -> 0.764 |
| q19 | 139.33 -> 138.35 | 0.993 | 1.016 -> 1.002 | 144.65 -> 145.35 | 1.005 | 1.019 -> 1.015 |
| q20 | 0.73 -> 0.59 | 0.808 | 0.631 -> 0.506 | 1.57 -> 1.48 | 0.943 | 0.340 -> 0.318 |
| q21 | 42.20 -> 41.98 | 0.995 | 1.007 -> 1.002 | 41.82 -> 41.76 | 0.999 | 1.000 -> 1.000 |
| q22 | 27.56 -> 20.55 | 0.746 | 1.001 -> 1.139 | 60.26 -> 58.55 | 0.972 | 0.992 -> 1.023 |
| q23 | 48.96 -> 39.39 | 0.805 | 1.002 -> 1.002 | 90.05 -> 89.69 | 0.996 | 0.994 -> 1.023 |
| q24 | 29.38 -> 27.75 | 0.945 | 0.997 -> 0.948 | 43.64 -> 43.06 | 0.987 | 0.976 -> 1.016 |
| q25 | 0.95 -> 0.95 | 1.000 | 1.049 -> 0.979 | 1045.10 -> 23.81 | 0.023 | 0.992 -> 0.858 |
| q26 | 10.91 -> 11.28 | 1.034 | 1.006 -> 1.044 | 1102.02 -> 76.64 | 0.070 | 0.997 -> 0.917 |
| q27 | 0.96 -> 0.81 | 0.844 | 0.957 -> 0.902 | 1057.73 -> 38.77 | 0.037 | 0.992 -> 0.753 |
| q28 | 67.00 -> 65.20 | 0.973 | 0.822 -> 0.814 | 87.55 -> 83.84 | 0.958 | 0.966 -> 0.964 |
| q29 | 349.10 -> 425.13 | 1.218 | 1.002 -> 1.224 | 356.29 -> 360.24 | 1.011 | 1.001 -> 0.999 |
| q30 | 0.02 -> 0.02 | - | 1.010 -> 1.027 | 276.13 -> 264.89 | 0.959 | 0.995 -> 1.001 |
| q31 | 51.61 -> 39.86 | 0.772 | 1.004 -> 0.939 | 56.22 -> 53.17 | 0.946 | 1.012 -> 1.019 |
| q32 | 59.28 -> 47.78 | 0.806 | 1.000 -> 0.962 | 71.43 -> 67.74 | 0.948 | 1.038 -> 1.032 |
| q33 | 212.53 -> 212.20 | 0.998 | 1.051 -> 1.044 | 217.37 -> 215.53 | 0.992 | 1.030 -> 1.009 |
| q34 | 38.68 -> 38.72 | 1.001 | 0.983 -> 0.990 | 43.57 -> 43.38 | 0.996 | 1.006 -> 0.998 |
| q35 | 38.64 -> 38.75 | 1.003 | 0.986 -> 0.985 | 43.37 -> 43.33 | 0.999 | 1.012 -> 1.004 |
| q36 | 61.05 -> 58.54 | 0.959 | 0.950 -> 0.917 | 144.73 -> 144.67 | 1.000 | 0.993 -> 0.994 |
| q37 | 16.53 -> 9.23 | 0.558 | 1.000 -> 0.549 | 34.42 -> 21.86 | 0.635 | 0.962 -> 0.603 |
| q38 | 14.81 -> 7.99 | 0.540 | 0.988 -> 0.529 | 33.22 -> 20.82 | 0.627 | 0.958 -> 0.579 |
| q39 | 7.19 -> 5.26 | 0.732 | 1.004 -> 0.731 | 13.24 -> 8.64 | 0.653 | 0.882 -> 0.567 |
| q40 | 58.61 -> 51.53 | 0.879 | 1.004 -> 0.876 | 115.19 -> 109.18 | 0.948 | 0.986 -> 0.929 |
| q41 | 5.69 -> 3.86 | 0.678 | 0.975 -> 0.672 | 17.17 -> 13.14 | 0.765 | 0.973 -> 0.743 |
| q42 | 7.54 -> 4.56 | 0.605 | 1.669 -> 1.012 | 16.36 -> 13.26 | 0.811 | 1.246 -> 1.007 |
| q43 | 8.93 -> 7.49 | 0.839 | 0.995 -> 0.834 | 26.56 -> 16.04 | 0.604 | 0.941 -> 0.563 |

The warm-time gains since P2 are spread across the filtered and aggregate
queries.  Splayed:
- q37 0.558, q38 0.540, q39 0.732, q41 0.678, q42 0.605;
- q18 0.729, q11 0.723, q12 0.713, q22 0.746, q31 0.772, q8 0.800.

Parted:
- the top-k queries q25 0.023, q26 0.070, q27 0.037;
- the directory answers q3, q4, q7 (now 0.00 ms);
- q8 0.341, q37 0.635, q43 0.604.

The one splayed query slower than at P2 is q29 (1.218).

### Gate items (first run, superseded by the rerun below)

**P3 gate (first run): FAIL (pending)**:
- the materialization counter is zero for filtered, rowsel-gather and
  aggregate shapes: the unit gates pass (`aware/gate_materialization`,
  planning included).  On ClickBench, `p3_unexpected_materializations`
  lists A/q23 SearchPhrase, B/q6 SearchPhrase and B/q28 URL: **FAIL**;
- decoded bytes are zero where statistics or codes answer, and exact
  otherwise: `aware/gate_decoded_bytes`, `aware/filter_*`,
  `aware/count_on_runs` and `aware/sum_*` pass;
- index row-list shapes materialize only the indexed column
  (`aware/gather_index_rows`), and top-k only its sort keys
  (`aware/topk_prune_cases`, `aware/gather_operators`, and the q24-q27
  traces above): pass;
- the cross-segment code tests pass (`aware/cross_segment_codes`,
  `aware/str_key_domain`);
- the conformance battery passes (Task 13).  The full suite at 7a603266 is
  5844 / 5844, with 0 UBSan runtime errors;
- the P2 gate's strict line: **FAIL**.  The splayed warm, first, cold and
  materializing-warm clauses fail (q13, q15, q29, q8, q22); gen-0 splayed
  warm fails on q27, below 1 ms; gen-0 parted and ragged answers fail on
  q40's tie order;
- RSS is within D3's bound: pass (x0.988 splayed, x0.984 parted).

Benchmarks.  Faster warm than generation 0: 23 of 43 splayed, 27 of 43
parted.  Faster cold: 37 of 43 splayed, 40 of 43 parted.  Slower warm by
more than 5%:
- splayed q13 1.414, q15 1.341, q29 1.224, q22 1.139, q8 1.128 (profiles
  above);
- the directory-answered q4-q7 (splayed) and q1 (parted), whose medians are
  0.00 ms on both sides.

### Per-query tables (summary.txt)

```
## arm A: splayed g0 vs g2 (ms; ratio = g2/g0)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.000	0.01	0.01	0.561	0.01	0.01	0.589	0.01	0.01	1.552	[]	same
q2	0.64	0.39	0.606	1.59	1.25	0.783	3.07	2.14	0.695	38.23	3.39	0.089	[]	same
q3	3.78	0.00	0.001	5.50	0.04	0.007	13.23	0.05	0.004	94.34	0.04	0.000	[]	same
q4	0.00	0.00	1.068	0.02	0.03	1.230	0.03	0.03	1.217	0.03	0.04	1.430	[]	same
q5	0.00	0.00	1.314	0.03	0.03	1.027	0.03	0.03	1.031	0.03	0.03	0.906	[]	same
q6	0.00	0.00	1.244	0.03	0.03	1.009	0.03	0.03	1.015	0.03	0.04	1.149	[]	same
q7	0.00	0.00	1.143	0.02	0.04	1.982	0.02	0.04	1.838	0.02	0.04	2.002	[]	same
q8	3.59	4.05	1.128	9.09	9.06	0.997	16.52	17.63	1.068	41.13	10.01	0.243	[]	same
q9	75.49	75.25	0.997	118.27	126.46	1.069	268.82	280.19	1.042	608.63	154.72	0.254	[RegionID UserID]	same
q10	69.50	71.13	1.024	99.27	116.77	1.176	239.28	268.10	1.120	410.25	153.27	0.374	[RegionID UserID]	same
q11	11.65	11.70	1.004	22.01	35.94	1.633	45.58	59.77	1.311	239.53	57.82	0.241	[UserID MobilePhoneModel]	same
q12	11.83	11.68	0.988	20.78	19.08	0.918	44.83	43.02	0.960	273.33	51.80	0.190	[]	same
q13	22.30	31.54	1.414	32.21	40.57	1.260	77.34	103.98	1.344	102.35	58.79	0.574	[]	same
q14	52.62	53.37	1.014	78.05	93.80	1.202	187.65	203.66	1.085	292.48	131.24	0.449	[UserID SearchPhrase]	same
q15	26.83	35.98	1.341	39.58	49.43	1.249	93.29	124.05	1.330	142.31	99.58	0.700	[]	same
q16	69.68	60.88	0.874	95.75	86.21	0.900	241.47	213.75	0.885	240.47	163.17	0.679	[]	same
q17	94.68	90.22	0.953	130.42	124.13	0.952	323.63	306.21	0.946	465.76	246.13	0.528	[]	same
q18	6.81	5.01	0.736	13.47	6.57	0.487	27.36	16.63	0.608	655.77	58.84	0.090	[]	same
q19	138.04	138.35	1.002	210.39	207.39	0.986	493.70	493.87	1.000	679.95	448.07	0.659	[EventTime]	same
q20	1.16	0.59	0.506	6.77	6.07	0.896	9.40	7.39	0.787	55.10	16.87	0.306	[]	same
q21	41.90	41.98	1.002	45.36	50.92	1.123	131.15	136.09	1.038	77.37	93.28	1.206	[URL]	same
q22	18.04	20.55	1.139	26.65	33.43	1.255	63.54	74.61	1.174	173.68	110.34	0.635	[URL]	same
q23	39.30	39.39	1.002	55.12	85.60	1.553	134.14	165.62	1.235	415.93	245.74	0.591	[Title UserID URL SearchPhrase]	same
q24	29.29	27.75	0.948	34.49	47.50	1.377	93.42	103.63	1.109	594.88	546.27	0.918	[EventTime]	same
q25	0.97	0.95	0.979	4.40	13.48	3.064	6.47	15.43	2.386	294.72	244.45	0.829	[EventTime]	same
q26	10.80	11.28	1.044	15.30	20.98	1.371	37.05	43.77	1.181	549.75	292.27	0.532	[SearchPhrase]	same
q27	0.90	0.81	0.902	4.43	19.07	4.306	6.44	20.93	3.249	290.37	251.87	0.867	[EventTime SearchPhrase]	same
q28	80.11	65.20	0.814	89.57	71.31	0.796	257.71	205.99	0.799	220.05	90.68	0.412	[]	same
q29	347.47	425.13	1.224	434.76	515.64	1.186	1133.70	1365.76	1.205	500.27	554.97	1.109	[]	same
q30	0.02	0.02	1.027	2.80	4.64	1.655	2.87	4.71	1.642	54.73	11.58	0.212	[ResolutionWidth]	same
q31	42.44	39.86	0.939	62.70	57.37	0.915	147.53	138.16	0.936	306.03	110.14	0.360	[]	same
q32	49.65	47.78	0.962	76.68	73.12	0.954	176.17	169.64	0.963	438.33	262.80	0.600	[]	same
q33	203.22	212.20	1.044	364.70	363.27	0.996	774.13	796.00	1.028	609.63	554.47	0.910	[]	same
q34	39.10	38.72	0.990	70.84	71.86	1.014	149.38	151.00	1.011	138.28	109.16	0.789	[]	same
q35	39.34	38.75	0.985	69.84	72.08	1.032	149.47	154.96	1.037	137.79	110.02	0.798	[]	same
q36	63.86	58.54	0.917	88.27	82.88	0.939	217.58	203.80	0.937	160.52	128.09	0.798	[]	same
q37	16.80	9.23	0.549	27.15	19.12	0.704	60.74	38.97	0.642	42.04	22.35	0.532	[]	same
q38	15.11	7.99	0.529	25.74	16.17	0.628	56.89	33.57	0.590	39.65	19.92	0.503	[]	same
q39	7.20	5.26	0.731	14.76	10.70	0.725	29.36	22.28	0.759	38.41	18.45	0.480	[]	same
q40	58.84	51.53	0.876	91.43	84.72	0.927	210.87	191.33	0.907	118.61	94.68	0.798	[]	same
q41	5.75	3.86	0.672	13.54	10.15	0.750	25.52	18.19	0.712	74.21	40.16	0.541	[]	same
q42	4.51	4.56	1.012	11.05	10.66	0.965	21.04	20.64	0.981	37.12	20.15	0.543	[]	same
q43	8.98	7.49	0.834	18.54	17.59	0.949	37.20	33.51	0.901	40.77	26.63	0.653	[]	same
totals over all 43: warm 1712.2 vs 1749.0 (1.021); first run 2531.4 vs 2675.2 (1.057); 3-run 6007.7 vs 6249.1 (1.040)
answer_mismatches none
order_differences none
scan_only(gated) n=30; gmeans over base>=1ms (per column): warm n=24 0.651, first n=25 0.738, cold n=25 0.357; worst warm=1.414
below_1ms_excluded_from_warm_gmean q1 q2 q4 q5 q6 q7
below_1ms_excluded_from_first_gmean q1 q4 q5 q6 q7
below_1ms_excluded_from_cold_gmean q1 q4 q5 q6 q7
mat rows missing none
scan_only_list_but_materializing none
measured_materializing q9(2 cols) q10(2 cols) q11(2 cols) q14(2 cols) q19(1 cols) q21(1 cols) q22(1 cols) q23(4 cols) q24(1 cols) q25(1 cols) q26(1 cols) q27(2 cols) q30(1 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q6(measured scan, list other) q12(measured scan, list other) q13(measured scan, list other) q15(measured scan, list other) q20(measured scan, list other) q28(measured scan, list other) q29(measured scan, list other) q31(measured scan, list other) q32(measured scan, list other) q36(measured scan, list other) q37(measured scan, list other) q38(measured scan, list other) q40(measured scan, list other) q41(measured scan, list other) q42(measured scan, list other) q43(measured scan, list other)
worst5 warm [(1.414, 'q13'), (1.341, 'q15'), (1.314, 'q5'), (1.244, 'q6'), (1.224, 'q29')]
worst5 cold [(2.002, 'q7'), (1.552, 'q1'), (1.43, 'q4'), (1.149, 'q6'), (1.109, 'q29')]
worst5 first [(1.982, 'q7'), (1.26, 'q13'), (1.249, 'q15'), (1.23, 'q4'), (1.186, 'q29')]
worst5 materializing_warm [(1.139, 'q22'), (1.044, 'q26'), (1.027, 'q30'), (1.024, 'q10'), (1.014, 'q14')]
first_run_total_ratio 1.057 (PI after P1: 2.99)
pi_row q41 warm 0.672 first 0.750 3run 0.712
pi_row q42 warm 1.012 first 0.965 3run 0.981

## arm B: parted p0 vs p2 (ms; ratio = p2/p0)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.052	0.01	0.01	0.976	0.01	0.01	0.981	0.01	0.01	0.581	[]	same
q2	6.75	6.43	0.953	16.81	16.35	0.973	30.37	29.32	0.965	45.63	16.32	0.358	[]	same
q3	532.99	0.00	0.000	548.07	0.05	0.000	1628.48	0.06	0.000	574.17	0.05	0.000	[]	same
q4	272.86	0.00	0.000	319.81	0.04	0.000	869.00	0.04	0.000	365.63	0.04	0.000	[]	same
q5	273.15	0.00	0.000	319.23	0.03	0.000	868.18	0.03	0.000	361.11	0.03	0.000	[]	same
q6	339.91	342.61	1.008	9704.33	9710.43	1.001	10384.72	10398.35	1.001	9764.99	9763.91	1.000	[SearchPhrase]	same
q7	527.02	0.00	0.000	555.42	0.05	0.000	1610.10	0.05	0.000	573.31	0.05	0.000	[]	same
q8	6.31	2.25	0.356	7.95	3.97	0.499	20.77	8.66	0.417	40.94	3.76	0.092	[]	same
q9	85.50	83.23	0.974	154.75	164.19	1.061	330.08	334.28	1.013	392.26	190.25	0.485	[RegionID UserID]	same
q10	78.79	79.37	1.007	153.99	167.29	1.086	313.14	337.93	1.079	440.67	196.62	0.446	[RegionID UserID]	same
q11	20.44	16.14	0.790	38.30	29.69	0.775	81.69	65.14	0.797	338.89	56.00	0.165	[]	same
q12	23.01	17.95	0.780	41.05	30.59	0.745	88.50	67.15	0.759	397.37	55.65	0.140	[]	same
q13	36.99	37.92	1.025	69.20	52.30	0.756	143.21	130.20	0.909	137.12	69.88	0.510	[]	same
q14	98.88	85.77	0.867	134.19	116.45	0.868	337.07	291.09	0.864	397.42	158.15	0.398	[]	same
q15	43.31	44.84	1.035	88.47	64.58	0.730	175.92	160.28	0.911	194.91	86.78	0.445	[]	same
q16	72.45	61.44	0.848	98.92	87.39	0.883	244.24	213.77	0.875	251.36	163.40	0.650	[]	same
q17	95.57	90.11	0.943	136.37	125.13	0.918	331.74	312.21	0.941	475.64	240.24	0.505	[]	same
q18	17.25	13.18	0.764	85.16	18.96	0.223	119.75	45.66	0.381	330.87	61.77	0.187	[]	same
q19	143.26	145.35	1.015	340.74	244.55	0.718	629.70	539.07	0.856	745.55	373.40	0.501	[EventTime]	same
q20	4.64	1.48	0.318	14.62	12.05	0.824	24.25	15.61	0.644	154.69	22.32	0.144	[]	same
q21	41.76	41.76	1.000	46.70	56.18	1.203	131.20	140.11	1.068	87.68	99.23	1.132	[URL]	same
q22	57.22	58.55	1.023	69.66	74.42	1.068	184.84	193.35	1.046	198.94	154.62	0.777	[URL]	same
q23	87.67	89.69	1.023	102.12	114.66	1.123	277.49	299.63	1.080	443.42	279.65	0.631	[Title URL]	same
q24	42.38	43.06	1.016	52.97	64.62	1.220	137.98	150.62	1.092	754.23	445.57	0.591	[URL]	same
q25	27.76	23.81	0.858	47.76	36.77	0.770	103.20	85.00	0.824	570.34	250.53	0.439	[]	same
q26	83.62	76.64	0.917	196.44	189.39	0.964	367.00	347.96	0.948	631.47	397.15	0.629	[]	same
q27	51.50	38.77	0.753	71.21	52.68	0.740	175.70	131.67	0.749	612.33	263.97	0.431	[]	same
q28	86.97	83.84	0.964	128.12	133.02	1.038	306.78	309.81	1.010	275.38	184.98	0.672	[URL]	same
q29	360.55	360.24	0.999	470.82	452.51	0.961	1197.06	1186.56	0.991	541.74	500.62	0.924	[]	same
q30	264.50	264.89	1.001	280.06	290.50	1.037	829.42	834.86	1.007	296.49	287.85	0.971	[ResolutionWidth]	same
q31	52.18	53.17	1.019	124.44	78.79	0.633	237.38	188.76	0.795	388.58	117.48	0.302	[]	same
q32	65.66	67.74	1.032	181.00	106.82	0.590	314.16	245.93	0.783	580.28	274.13	0.472	[]	same
q33	213.58	215.53	1.009	376.52	385.70	1.024	808.07	830.19	1.027	612.29	637.07	1.040	[]	same
q34	43.49	43.38	0.998	95.86	75.46	0.787	183.59	163.62	0.891	175.37	119.33	0.680	[]	same
q35	43.14	43.33	1.004	96.24	75.96	0.789	184.67	166.80	0.903	177.87	119.13	0.670	[]	same
q36	145.50	144.67	0.994	214.78	198.11	0.922	511.90	495.09	0.967	302.06	208.95	0.692	[]	same
q37	36.25	21.86	0.603	70.06	37.17	0.531	142.85	83.19	0.582	230.61	83.01	0.360	[]	same
q38	35.95	20.82	0.579	69.57	38.06	0.547	145.73	81.02	0.556	230.92	82.49	0.357	[]	same
q39	15.22	8.64	0.567	27.59	20.42	0.740	59.01	37.75	0.640	122.33	26.11	0.213	[]	same
q40	117.47	109.18	0.929	427.83	350.02	0.818	665.59	574.86	0.864	783.75	442.62	0.565	[]	same
q41	17.68	13.14	0.743	39.28	26.75	0.681	74.72	54.43	0.728	263.31	176.16	0.669	[]	same
q42	13.17	13.26	1.007	23.71	23.86	1.006	50.85	54.62	1.074	191.60	147.31	0.769	[]	same
q43	28.49	16.04	0.563	41.11	27.63	0.672	98.10	60.97	0.621	135.74	35.19	0.259	[]	same
totals over all 43: warm 4610.8 vs 2880.1 (0.625); first run 16081.3 vs 13753.6 (0.855); 3-run 25418.2 vs 19665.7 (0.774)
answer_mismatches none
order_differences none
scan_only(gated) n=33; gmeans over base>=1ms (per column): warm n=32 0.170, first n=32 0.241, cold n=32 0.140; worst warm=1.052
below_1ms_excluded_from_warm_gmean q1
below_1ms_excluded_from_first_gmean q1
below_1ms_excluded_from_cold_gmean q1
mat rows missing none
scan_only_list_but_materializing none
measured_materializing q6(1 cols) q9(2 cols) q10(2 cols) q19(1 cols) q21(1 cols) q22(1 cols) q23(2 cols) q24(1 cols) q28(1 cols) q30(1 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q11(measured scan, list other) q12(measured scan, list other) q13(measured scan, list other) q14(measured scan, list other) q15(measured scan, list other) q20(measured scan, list other) q25(measured scan, list other) q26(measured scan, list other) q27(measured scan, list other) q29(measured scan, list other) q31(measured scan, list other) q32(measured scan, list other) q36(measured scan, list other) q37(measured scan, list other) q38(measured scan, list other) q40(measured scan, list other) q41(measured scan, list other) q42(measured scan, list other) q43(measured scan, list other)
worst5 warm [(1.052, 'q1'), (1.035, 'q15'), (1.032, 'q32'), (1.025, 'q13'), (1.019, 'q31')]
worst5 cold [(1.04, 'q33'), (0.924, 'q29'), (0.769, 'q42'), (0.692, 'q36'), (0.68, 'q34')]
worst5 first [(1.024, 'q33'), (1.006, 'q42'), (0.976, 'q1'), (0.973, 'q2'), (0.964, 'q26')]
worst5 materializing_warm [(1.023, 'q23'), (1.023, 'q22'), (1.016, 'q24'), (1.015, 'q19'), (1.008, 'q6')]
pi_row q41 warm 0.743 first 0.681 3run 0.728
pi_row q42 warm 1.007 first 1.006 3run 1.074

## arm C0: splayed g0, base vs p2 binary (ms; ratio = p2/base)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	0.989	0.01	0.01	1.009	0.01	0.01	1.006	nan	nan	nan		same
q2	1.11	0.64	0.576	2.01	1.67	0.830	4.41	3.13	0.709	nan	nan	nan		same
q3	3.78	3.79	1.002	5.83	5.85	1.004	13.68	13.47	0.985	nan	nan	nan		same
q4	0.00	0.00	1.018	0.02	0.02	1.023	0.02	0.02	1.027	nan	nan	nan		same
q5	0.00	0.00	1.014	0.03	0.03	1.153	0.03	0.03	1.141	nan	nan	nan		same
q6	0.00	0.00	1.059	0.03	0.03	1.155	0.03	0.03	1.146	nan	nan	nan		same
q7	0.00	0.00	1.005	0.02	0.02	1.025	0.02	0.02	1.029	nan	nan	nan		same
q8	3.51	3.60	1.025	9.02	9.13	1.012	16.32	16.48	1.010	nan	nan	nan		same
q9	75.82	75.11	0.991	114.42	117.44	1.026	272.55	268.70	0.986	nan	nan	nan		same
q10	69.40	69.03	0.995	97.93	98.25	1.003	243.15	238.14	0.979	nan	nan	nan		same
q11	16.16	11.56	0.715	25.50	22.21	0.871	58.57	45.94	0.784	nan	nan	nan		same
q12	16.33	11.87	0.727	25.84	20.82	0.806	59.60	45.00	0.755	nan	nan	nan		same
q13	31.95	22.65	0.709	41.14	32.17	0.782	105.35	77.59	0.737	nan	nan	nan		same
q14	62.66	53.25	0.850	87.92	78.04	0.888	218.26	186.88	0.856	nan	nan	nan		same
q15	36.58	26.48	0.724	48.60	39.72	0.817	126.01	93.49	0.742	nan	nan	nan		same
q16	70.03	69.61	0.994	95.84	94.27	0.984	237.68	234.65	0.987	nan	nan	nan		same
q17	93.33	93.28	0.999	129.64	130.37	1.006	322.60	317.00	0.983	nan	nan	nan		same
q18	6.93	6.92	0.998	14.98	15.02	1.003	28.95	29.38	1.015	nan	nan	nan		same
q19	137.39	138.02	1.005	208.71	212.28	1.017	489.99	492.37	1.005	nan	nan	nan		same
q20	1.12	1.13	1.009	6.88	7.10	1.031	9.48	9.60	1.013	nan	nan	nan		same
q21	41.75	42.02	1.006	45.48	46.24	1.017	130.68	132.69	1.015	nan	nan	nan		same
q22	27.70	18.12	0.654	32.01	26.55	0.829	88.49	63.38	0.716	nan	nan	nan		same
q23	48.89	39.22	0.802	60.72	55.16	0.908	158.90	134.07	0.844	nan	nan	nan		same
q24	29.16	29.51	1.012	35.42	34.42	0.972	97.06	95.06	0.979	nan	nan	nan		same
q25	0.94	0.91	0.967	4.48	4.50	1.004	6.40	6.49	1.015	nan	nan	nan		same
q26	10.78	10.82	1.003	15.44	15.68	1.015	37.04	37.72	1.018	nan	nan	nan		same
q27	0.90	1.00	1.110	4.48	4.46	0.995	6.46	6.60	1.023	nan	nan	nan		same
q28	82.75	80.21	0.969	84.94	86.76	1.021	257.95	248.18	0.962	nan	nan	nan		same
q29	356.59	345.68	0.969	450.96	436.37	0.968	1174.50	1138.82	0.970	nan	nan	nan		same
q30	0.02	0.03	1.773	3.02	3.00	0.994	3.08	3.05	0.989	nan	nan	nan		same
q31	52.72	42.10	0.799	71.65	61.02	0.852	178.12	146.52	0.823	nan	nan	nan		same
q32	59.36	49.48	0.833	86.30	79.12	0.917	208.20	183.31	0.880	nan	nan	nan		same
q33	202.80	204.42	1.008	369.58	361.75	0.979	782.89	785.95	1.004	nan	nan	nan		same
q34	39.55	39.18	0.990	70.45	71.60	1.016	150.64	150.05	0.996	nan	nan	nan		same
q35	39.46	39.25	0.995	70.49	69.75	0.989	157.26	148.98	0.947	nan	nan	nan		same
q36	63.90	64.17	1.004	87.56	87.05	0.994	218.99	216.70	0.990	nan	nan	nan		same
q37	17.20	16.61	0.966	27.95	27.24	0.974	62.51	60.64	0.970	nan	nan	nan		same
q38	15.68	15.43	0.984	25.29	24.58	0.972	57.42	55.78	0.971	nan	nan	nan		same
q39	7.48	7.19	0.961	14.74	14.65	0.994	30.15	29.20	0.968	nan	nan	nan		same
q40	58.87	58.83	0.999	89.87	90.75	1.010	210.31	210.83	1.002	nan	nan	nan		same
q41	5.81	5.76	0.993	13.31	13.49	1.014	25.25	25.26	1.001	nan	nan	nan		same
q42	4.65	4.40	0.945	11.07	11.15	1.007	20.57	20.70	1.006	nan	nan	nan		same
q43	9.31	9.06	0.973	18.82	18.38	0.977	40.33	37.58	0.932	nan	nan	nan		same
totals over all 43: warm 1802.3 vs 1710.3 (0.949); first run 2608.4 vs 2528.1 (0.969); 3-run 6309.9 vs 6009.5 (0.952)
answer_mismatches none
order_differences none
gen0 warm_gmean(base>=1ms n=35)=0.910 warm_worst=1.773 worst5 [(1.773, 'q30'), (1.11, 'q27'), (1.059, 'q6'), (1.025, 'q8'), (1.018, 'q4')]

## arm C1: parted p0, base vs p2 binary (ms; ratio = p2/base)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.168	0.01	0.01	1.659	0.01	0.01	1.628	nan	nan	nan		same
q2	6.55	6.69	1.022	17.20	16.91	0.983	30.47	30.55	1.003	nan	nan	nan		same
q3	531.18	534.29	1.006	558.45	548.30	0.982	1636.17	1622.64	0.992	nan	nan	nan		same
q4	277.78	270.61	0.974	331.31	319.15	0.963	886.86	868.90	0.980	nan	nan	nan		same
q5	277.98	273.13	0.983	328.60	321.27	0.978	892.74	869.66	0.974	nan	nan	nan		same
q6	342.94	342.45	0.999	9721.68	9706.65	0.998	10414.78	10393.52	0.998	nan	nan	nan		same
q7	527.64	523.20	0.992	555.78	551.49	0.992	1610.99	1613.99	1.002	nan	nan	nan		same
q8	9.19	6.41	0.697	21.42	7.92	0.370	39.95	20.91	0.524	nan	nan	nan		same
q9	88.90	85.11	0.957	179.93	151.86	0.844	360.03	324.32	0.901	nan	nan	nan		same
q10	87.26	78.65	0.901	203.74	152.91	0.751	383.84	316.04	0.823	nan	nan	nan		same
q11	358.46	20.23	0.056	596.91	38.22	0.064	1321.14	81.00	0.061	nan	nan	nan		same
q12	361.36	22.66	0.063	592.09	40.32	0.068	1317.56	86.38	0.066	nan	nan	nan		same
q13	40.07	36.80	0.918	69.60	70.25	1.009	150.93	145.26	0.962	nan	nan	nan		same
q14	1135.50	98.14	0.086	1421.63	136.09	0.096	3706.61	337.04	0.091	nan	nan	nan		same
q15	46.44	43.07	0.927	89.30	90.06	1.009	184.30	178.61	0.969	nan	nan	nan		same
q16	76.32	72.72	0.953	147.98	97.44	0.659	305.43	244.71	0.801	nan	nan	nan		same
q17	103.93	96.16	0.925	207.04	133.39	0.644	418.69	329.98	0.788	nan	nan	nan		same
q18	17.23	17.11	0.993	84.69	83.96	0.991	120.69	118.09	0.978	nan	nan	nan		same
q19	141.17	143.41	1.016	333.40	338.59	1.016	624.15	629.11	1.008	nan	nan	nan		same
q20	4.63	4.67	1.007	14.45	14.35	0.993	24.05	23.90	0.994	nan	nan	nan		same
q21	41.80	41.87	1.002	47.31	46.70	0.987	130.78	131.59	1.006	nan	nan	nan		same
q22	60.42	57.13	0.945	85.91	69.22	0.806	209.53	184.53	0.881	nan	nan	nan		same
q23	93.83	86.82	0.925	214.63	101.17	0.471	405.77	276.54	0.682	nan	nan	nan		same
q24	44.54	42.46	0.953	135.14	52.17	0.386	225.82	139.81	0.619	nan	nan	nan		same
q25	1074.76	27.44	0.026	1345.66	46.19	0.034	3501.16	101.40	0.029	nan	nan	nan		same
q26	1131.50	83.50	0.074	1501.25	195.22	0.130	3763.66	363.50	0.097	nan	nan	nan		same
q27	1086.70	51.36	0.047	1359.10	69.76	0.051	3542.68	174.50	0.049	nan	nan	nan		same
q28	92.07	86.96	0.944	148.73	128.95	0.867	335.79	303.46	0.904	nan	nan	nan		same
q29	363.91	358.75	0.986	473.12	467.32	0.988	1210.94	1194.90	0.987	nan	nan	nan		same
q30	266.48	263.67	0.989	282.45	278.91	0.987	831.26	819.76	0.986	nan	nan	nan		same
q31	55.79	51.86	0.930	122.18	123.21	1.008	236.81	234.04	0.988	nan	nan	nan		same
q32	69.13	65.57	0.949	177.54	179.40	1.010	317.70	316.87	0.997	nan	nan	nan		same
q33	218.46	213.21	0.976	462.22	371.26	0.803	912.23	813.09	0.891	nan	nan	nan		same
q34	43.38	42.96	0.990	96.01	98.13	1.022	184.36	185.42	1.006	nan	nan	nan		same
q35	389.48	42.94	0.110	2307.75	96.61	0.042	3084.96	185.00	0.060	nan	nan	nan		same
q36	148.19	145.38	0.981	215.49	217.27	1.008	517.26	514.67	0.995	nan	nan	nan		same
q37	36.67	36.12	0.985	69.54	68.33	0.982	145.21	141.25	0.973	nan	nan	nan		same
q38	35.45	35.22	0.994	68.43	67.82	0.991	146.19	144.36	0.987	nan	nan	nan		same
q39	15.38	15.10	0.982	27.27	27.13	0.995	58.31	58.22	0.998	nan	nan	nan		same
q40	117.61	117.90	1.002	421.18	423.15	1.005	663.45	664.41	1.001	nan	nan	nan		DIFF
q41	17.63	17.55	0.995	38.74	38.73	1.000	74.52	74.10	0.994	nan	nan	nan		same
q42	13.47	13.19	0.980	24.37	23.80	0.977	51.46	51.13	0.994	nan	nan	nan		same
q43	28.45	28.23	0.992	41.48	41.05	0.989	101.26	98.83	0.976	nan	nan	nan		same
totals over all 43: warm 9879.7 vs 4600.7 (0.466); first run 25140.7 vs 16050.6 (0.638); 3-run 45080.5 vs 25406.0 (0.564)
answer_mismatches q40:DIFF
order_differences none
gen0 warm_gmean(base>=1ms n=42)=0.607 warm_worst=1.168 worst5 [(1.168, 'q1'), (1.022, 'q2'), (1.016, 'q19'), (1.007, 'q20'), (1.006, 'q3')]
q16 first run base 148.0 ms, p2 97.4 ms (carry 2)

## arm R: parted ragged, base vs p2 binary (ms; ratio = p2/base)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.018	0.01	0.01	1.043	0.01	0.01	1.041	nan	nan	nan		same
q2	7.81	7.58	0.970	18.24	17.47	0.958	33.82	32.90	0.973	nan	nan	nan		same
q3	582.34	269.71	0.463	598.13	286.38	0.479	1769.76	830.03	0.469	nan	nan	nan		same
q4	276.11	273.71	0.991	325.57	316.65	0.973	881.32	867.24	0.984	nan	nan	nan		same
q5	277.60	271.26	0.977	325.52	323.44	0.994	884.05	863.94	0.977	nan	nan	nan		same
q6	344.27	340.36	0.989	9731.98	9744.37	1.001	10453.01	10432.31	0.998	nan	nan	nan		same
q7	526.78	523.55	0.994	552.98	552.63	0.999	1620.81	1604.75	0.990	nan	nan	nan		same
q8	9.58	8.77	0.915	22.33	10.32	0.462	41.77	27.88	0.668	nan	nan	nan		same
q9	89.20	85.31	0.956	178.64	154.91	0.867	361.46	330.29	0.914	nan	nan	nan		same
q10	87.28	79.38	0.909	204.49	154.54	0.756	384.14	315.69	0.822	nan	nan	nan		same
q11	379.16	19.95	0.053	663.10	36.74	0.055	1422.76	77.06	0.054	nan	nan	nan		same
q12	376.05	22.56	0.060	661.86	39.83	0.060	1414.53	85.41	0.060	nan	nan	nan		same
q13	39.96	36.74	0.919	71.42	70.54	0.988	152.46	144.43	0.947	nan	nan	nan		same
q14	1191.29	100.21	0.084	1526.78	134.44	0.088	3921.24	337.68	0.086	nan	nan	nan		same
q15	46.49	42.87	0.922	89.50	88.20	0.985	186.38	174.72	0.937	nan	nan	nan		same
q16	76.22	72.72	0.954	145.99	103.06	0.706	301.88	250.89	0.831	nan	nan	nan		same
q17	103.27	95.81	0.928	203.53	134.01	0.658	411.28	327.71	0.797	nan	nan	nan		same
q18	17.02	16.99	0.998	83.65	83.54	0.999	118.48	118.08	0.997	nan	nan	nan		same
q19	141.58	143.21	1.012	336.17	339.45	1.010	623.27	630.08	1.011	nan	nan	nan		same
q20	4.46	4.47	1.001	14.34	14.27	0.995	23.55	23.62	1.003	nan	nan	nan		same
q21	41.80	41.90	1.002	46.35	46.63	1.006	131.44	130.91	0.996	nan	nan	nan		same
q22	60.24	56.86	0.944	85.16	69.43	0.815	206.76	184.57	0.893	nan	nan	nan		same
q23	93.95	86.72	0.923	276.29	101.92	0.369	471.20	279.65	0.593	nan	nan	nan		same
q24	44.44	42.61	0.959	196.59	51.75	0.263	293.39	136.94	0.467	nan	nan	nan		same
q25	1130.54	27.65	0.024	1444.58	44.69	0.031	3720.06	100.78	0.027	nan	nan	nan		same
q26	1186.13	83.17	0.070	1594.96	197.36	0.124	3975.01	365.39	0.092	nan	nan	nan		same
q27	1142.81	51.62	0.045	1467.56	70.03	0.048	3752.06	175.32	0.047	nan	nan	nan		same
q28	92.13	86.64	0.940	148.17	125.80	0.849	334.51	302.53	0.904	nan	nan	nan		same
q29	364.79	358.93	0.984	474.70	471.02	0.992	1213.72	1201.51	0.990	nan	nan	nan		same
q30	267.16	265.30	0.993	281.35	280.43	0.997	834.16	833.52	0.999	nan	nan	nan		same
q31	55.26	51.89	0.939	125.77	122.81	0.976	237.51	236.77	0.997	nan	nan	nan		same
q32	69.00	65.60	0.951	176.83	176.68	0.999	319.67	310.37	0.971	nan	nan	nan		same
q33	217.79	213.02	0.978	469.57	381.03	0.811	917.57	813.04	0.886	nan	nan	nan		same
q34	43.26	43.11	0.997	95.99	96.08	1.001	185.92	183.58	0.987	nan	nan	nan		same
q35	412.19	43.17	0.105	2415.11	94.99	0.039	3246.49	183.31	0.056	nan	nan	nan		same
q36	148.93	146.04	0.981	215.76	216.21	1.002	525.13	513.53	0.978	nan	nan	nan		same
q37	36.36	33.02	0.908	72.94	66.21	0.908	147.72	133.17	0.901	nan	nan	nan		same
q38	35.06	32.13	0.916	73.33	64.97	0.886	147.71	137.17	0.929	nan	nan	nan		same
q39	15.21	12.51	0.823	29.11	24.68	0.848	60.02	49.91	0.832	nan	nan	nan		same
q40	153.14	148.95	0.973	463.08	452.18	0.976	772.32	752.17	0.974	nan	nan	nan		DIFF
q41	17.60	17.16	0.976	40.71	38.13	0.937	76.46	73.68	0.964	nan	nan	nan		same
q42	13.29	12.44	0.936	26.61	22.68	0.852	53.36	47.83	0.896	nan	nan	nan		same
q43	28.12	26.05	0.926	42.45	39.02	0.919	100.90	91.37	0.905	nan	nan	nan		same
totals over all 43: warm 10245.7 vs 4361.6 (0.426); first run 26017.2 vs 15859.5 (0.610); 3-run 46729.1 vs 24711.7 (0.529)
answer_mismatches q40:DIFF
order_differences none
gen0 warm_gmean(base>=1ms n=42)=0.588 warm_worst=1.018 worst5 [(1.018, 'q1'), (1.012, 'q19'), (1.002, 'q21'), (1.001, 'q20'), (0.998, 'q18')]

enc A: columns with DELTA vectors: ['UserID {raw:76 const:3 delta_bp:97 rle:9526 dict:64}]
enc B: columns with DELTA vectors: ['UserID {raw:75 const:3 delta_bp:89 rle:8963 dict:640}]
sizes {('A', 'g0'): 5948677939, ('A', 'g2'): 3307128731, ('B', 'p0'): 4807844987, ('B', 'p2'): 2072366907}  peak_rss_kb {('A', 'g0'): 5120100, ('A', 'g2'): 5060388, ('B', 'p0'): 12158788, ('B', 'p2'): 11963400}  evict_modes fadvise  ragged_partition ['9', '40']  meta {'p2': ['7a603266', '95039693ca59'], 'base': ['80a28e49', 'f1075b366780']}
GATE (strict) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=FAIL warm=PASS q16_first=PASS] gen0-ragged[answers=FAIL warm=PASS]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=FAIL cold=FAIL first=FAIL materializing_warm=FAIL no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=FAIL warm=PASS q16_first=PASS] gen0-ragged[answers=FAIL warm=PASS]
P2 GATE (P3-exempt): FAIL
P3-EXEMPT LIST q8/A/warm[run-level compute (group over RLE runs)]: 3.59 -> 4.05 ms (x1.128) EXEMPTED; q8/A/first[run-level compute (group over RLE runs, first run)]: 9.09 -> 9.06 ms (x0.997) within-bound; q18/A/first[row-list gather (first-n keys published, P3 path 2)]: 13.47 -> 6.57 ms (x0.487) within-bound; q39/A/first[selection gather (P3 path 2)]: 14.76 -> 10.70 ms (x0.725) within-bound; q39/B/first[selection gather (P3 path 2)]: 27.59 -> 20.42 ms (x0.740) within-bound; q39/A/cold[selection gather reads whole columns cold (P3)]: 38.41 -> 18.45 ms (x0.480) within-bound; q42/A/warm[point gathers of the selected keys (P3)]: 4.51 -> 4.56 ms (x1.012) within-bound; q42/B/warm[point gathers of the selected keys (P3)]: 13.17 -> 13.26 ms (x1.007) within-bound; q24/A/rss[top-k output gather RSS]: arm peak-RSS 5120100 -> 5060388 kB (x0.99, max 2.37) not gated; q25/A/rss[top-k output gather RSS]: arm peak-RSS 5120100 -> 5060388 kB (x0.99, max 2.37) not gated; q26/A/rss[top-k output gather RSS]: arm peak-RSS 5120100 -> 5060388 kB (x0.99, max 2.37) not gated; q27/A/rss[top-k output gather RSS]: arm peak-RSS 5120100 -> 5060388 kB (x0.99, max 2.37) not gated
p3_unexpected_materializations A/q23:SearchPhrase B/q6:SearchPhrase B/q28:URL
rss A: 5120100 -> 5060388 kB (x0.988, bound 1.1)
rss B: 12158788 -> 11963400 kB (x0.984, bound 1.1)
carry q2/A/warm: P2 x1.086 -> P3 x0.606
carry q8/A/warm: P2 x1.424 -> P3 x1.128
carry q8/B/warm: P2 x1.060 -> P3 x0.356
carry q18/A/first: P2 x1.983 -> P3 x0.487
carry q18/B/warm: P2 x1.088 -> P3 x0.764
carry q39/A/first: P2 x2.402 -> P3 x0.725
carry q39/B/first: P2 x1.503 -> P3 x0.740
carry q42/A/warm: P2 x1.664 -> P3 x1.012
carry q42/B/warm: P2 x1.237 -> P3 x1.007
P3GATE p2_strict=FAIL materialization=FAIL rss=PASS
P3 GATE: FAIL
```

### Gate rerun (HEAD 2f949edc, after Task 14b)

This is the same driver and method, `W=/home/hetoku/data/tmp/p3_bench2
p2_bench.sh all`, run 17:03-18:45.  The binary is HEAD 2f949edc (release,
sha256 f98fef6700b4) and the base is 80a28e49 (f1075b366780).  The load was
0.82 at the start.  The CI runner's listener was idle, no Runner.Worker
process appeared, and nothing else heavy ran.  14b's data edits are in
force: `INTENDED_ANSWER_CHANGES` (q40 on C1 and R, #801) and
`ALLOW_ARM` (A/q23, B/q6 SearchPhrase).  Raw data:
/home/hetoku/data/tmp/p3_bench2/p2.tsv, with summary.txt beside it.

```
GATE (strict) splayed[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (strict): FAIL
p3_unexpected_materializations none
rss A: 5108136 -> 5054232 kB (x0.989, bound 1.1)
rss B: 12164164 -> 11963740 kB (x0.984, bound 1.1)
P3GATE p2_strict=FAIL materialization=PASS rss=PASS
P3 GATE: FAIL
```

**P3 gate: PASS** (Ruling 14-8: the only failing clause, gen-0 splayed C0 q25 at 1.055 by best round, 0.002 ms over the floor, is sub-ms noise; medians 1.011 vs 0.994, identical perf)

The clause in detail: gen0-splayed warm, C0 q25 (base against p2 on g0),
0.92 -> 0.97 ms, best-round ratio 1.055, a gap of 0.052 ms against the
0.05 ms floor.
- The medians of the rounds are base 1.011 and p2 0.994 ms.
- A 9-round alternated re-check on a rebuilt g0 gave medians of base 1.010
  and p2 0.998 ms, and best rounds of 0.939 and 0.874.
- `perf record -g`, 200 runs in one process, top symbols:
  - base: fpk_scan_rows 2 658, fpk_cmp 2 249, dom_parse_strl_atoms 790,
    fp_eval_cmp 310, worker_loop 84;
  - p2: fpk_par_fn_plain 2 762, fpk_cmp 2 166, dom_parse_strl_atoms 791,
    fp_eval_cmp 311, worker_loop 95.
- The work is the same.  This is the sub-ms best-round artifact of ruling
  14-1, now landing on q25: C0 q27, the first run's case, is at 0.909 here.
  The gen-0 splayed warm gmean is 0.911.

Every other clause passes:
- the splayed and parted strict lines, every field;
- gen-0 parted and ragged, with the q40 tie order an intended change;
- `p3_unexpected_materializations none`;
- RSS x0.989 / x0.984;
- every carry within its bound (the worst is q42/A warm at 1.032).

| query, arm | first run (7a603266) | rerun (2f949edc) |
|---|---|---|
| q8 A warm | 1.128 | 0.817 |
| q13 A warm / first | 1.414 / 1.260 | 1.044 / 0.995 |
| q15 A warm / first | 1.341 / 1.249 | 1.026 / 1.021 |
| q22 A warm (materializing) / first | 1.139 / 1.255 | 0.985 / 1.156 |
| q29 A warm / first / cold | 1.224 / 1.186 / 1.109 | 1.005 / 1.015 / 0.961 |
| B q28 materialized | URL | none |
| C0 q27 / q25 warm | 1.110 / 0.967 | 0.909 / 1.055 |

The strict line counts q22 among the materializing queries: its `mat` row
is [URL], the LIKE filter.  So only its warm clause (1.05) is gated, and it
passes at 0.985.  Its first run of 1.156 carries URL's first-touch decode,
which is reported and not gated.  q15 warm is 1.026 (bound 1.05).

| watch item | first run | rerun |
|---|---|---|
| q8 splayed warm | 1.128 | 0.817 |
| q10 splayed warm / first; parted warm | 1.024 / 1.176; 1.007 | 1.036 / 1.169; 1.007 |
| q26 splayed warm | 1.044 | 1.046 |
| parted q42 warm / first / 3-run | 1.007 / 1.006 / 1.074 | 0.947 / 0.996 / 1.059 |
| arm R warm gmean, worst | 0.588, 1.018 (q1) | 0.589, 1.014 (q7, below 1 ms); q40 intended |
| parted q4 (p0 -> p2 ms); gen 0 C1 / R | 272.86 -> 0.00; 0.974 / 0.991 | 273.79 -> 0.00; 0.996 / 0.996 |
| q24 URL first-touch, splayed / parted first | 1.377 / 1.220 | 1.364 / 1.221 |
| q24-q27 first, splayed | 1.377 / 3.064 / 1.371 / 4.306 | 1.364 / 3.073 / 1.376 / 4.359 |
| q24-q27 first, parted | 1.220 / 0.770 / 0.964 / 0.740 | 1.221 / 0.770 / 0.937 / 0.740 |
| SYM / integer `in` later in a chain, splayed g2 | materializes the column | 0 (both shapes; same counts) |
| parted `in`, first and later | 0 | 0 |

#### Gate items (rerun)

**P3 gate: PASS** (Ruling 14-8):
- the materialization counter is zero for filtered, rowsel-gather and
  aggregate shapes (`aware/gate_materialization`, planning included), and
  `p3_unexpected_materializations none` on ClickBench;
- decoded bytes are zero where statistics or codes answer, and exact
  otherwise (`aware/gate_decoded_bytes`, `aware/filter_*`,
  `aware/count_on_runs`, `aware/sum_*`);
- index row-list shapes materialize only the indexed column
  (`aware/gather_index_rows`), and top-k only its sort keys
  (`aware/topk_prune_cases`, `aware/gather_operators`, and the q24-q27
  materialization lines: EventTime and SearchPhrase only);
- the cross-segment code tests pass (`aware/cross_segment_codes`,
  `aware/str_key_domain`);
- the conformance battery passes (Task 13; full suite 5851/5851 on the 14b
  tree, 0 UBSan runtime errors);
- the P2 gate's strict line passes, with every P2 deferral cleared.  Every
  field passes except gen-0 splayed warm C0 q25, which Ruling 14-8 rules
  sub-ms noise; q40 on C1 and R is an intended change (ruling 14-2);
- RSS is within D3's bound (x0.989 splayed, x0.984 parted).

Benchmarks (generation 2 against generation 0, same binary, all 43
queries).  Faster warm: 28 of 43 splayed, 27 of 43 parted.  Faster cold:
37 of 43 splayed, 40 of 43 parted.

Slower warm by more than 5%: only the directory-answered queries whose
medians are 0.00 ms on both sides:
- splayed q4 1.064, q5 1.272, q6 1.276, q7 1.150;
- parted q1 1.109.

All of these pass on the floor.  Slower than 1.05 by first run: splayed
q24 1.364, q25 3.073, q26 1.376, q27 4.359 (the top-k sort keys'
first-touch decode), q10 1.169 and q22 1.156 (materializing; first touch of
the materialized columns).  These are reported, not gated.


#### Per-query tables (rerun summary.txt)

```
## arm A: splayed g0 vs g2 (ms; ratio = g2/g0)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	0.886	0.01	0.01	0.992	0.01	0.01	0.990	0.01	0.01	1.044	[]	same
q2	0.65	0.40	0.617	1.55	1.21	0.782	3.05	2.11	0.690	38.17	3.29	0.086	[]	same
q3	3.80	0.00	0.001	5.53	0.04	0.007	13.20	0.05	0.003	93.63	0.04	0.000	[]	same
q4	0.00	0.00	1.064	0.03	0.03	1.379	0.03	0.04	1.370	0.03	0.03	1.278	[]	same
q5	0.00	0.00	1.272	0.03	0.03	1.144	0.03	0.04	1.145	0.03	0.03	1.140	[]	same
q6	0.00	0.00	1.276	0.03	0.03	1.118	0.03	0.04	1.118	0.03	0.03	1.139	[]	same
q7	0.00	0.00	1.150	0.02	0.04	1.802	0.02	0.04	1.688	0.02	0.04	1.877	[]	same
q8	3.58	2.92	0.817	9.08	8.30	0.913	16.22	14.33	0.883	41.43	9.08	0.219	[]	same
q9	75.37	75.25	0.998	114.72	124.72	1.087	267.79	281.19	1.050	654.58	155.11	0.237	[RegionID UserID]	same
q10	68.58	71.04	1.036	98.53	115.17	1.169	236.49	264.33	1.118	403.70	154.59	0.383	[RegionID UserID]	same
q11	11.74	11.68	0.994	21.93	36.02	1.643	45.30	59.56	1.315	237.70	58.02	0.244	[UserID MobilePhoneModel]	same
q12	12.00	9.59	0.799	20.53	17.77	0.866	44.95	37.28	0.829	273.22	50.42	0.185	[]	same
q13	22.21	23.19	1.044	32.15	31.99	0.995	77.10	78.56	1.019	100.78	49.79	0.494	[]	same
q14	52.56	52.83	1.005	78.04	93.84	1.202	187.74	203.90	1.086	291.23	131.26	0.451	[UserID SearchPhrase]	same
q15	26.65	27.35	1.026	39.51	40.33	1.021	92.69	96.79	1.044	142.87	88.28	0.618	[]	same
q16	70.03	61.03	0.871	96.09	92.08	0.958	240.61	216.67	0.901	239.91	169.09	0.705	[]	same
q17	93.41	89.94	0.963	129.84	126.13	0.971	321.28	311.38	0.969	453.42	256.55	0.566	[]	same
q18	6.82	4.25	0.623	13.49	5.92	0.439	27.22	14.65	0.538	638.11	59.48	0.093	[]	same
q19	138.17	138.20	1.000	209.32	207.19	0.990	495.98	488.60	0.985	675.24	440.93	0.653	[EventTime]	same
q20	1.14	0.62	0.543	6.98	5.84	0.837	9.68	7.25	0.749	55.38	16.90	0.305	[]	same
q21	41.88	41.85	0.999	45.22	50.44	1.115	129.16	134.68	1.043	77.28	92.95	1.203	[URL]	same
q22	18.02	17.76	0.985	26.52	30.64	1.156	63.11	66.14	1.048	172.98	109.62	0.634	[URL]	same
q23	39.20	39.41	1.005	55.63	80.00	1.438	134.52	161.36	1.200	418.28	248.75	0.595	[Title UserID URL SearchPhrase]	same
q24	29.40	27.76	0.944	34.97	47.69	1.364	93.82	103.34	1.102	589.03	542.51	0.921	[EventTime]	same
q25	0.93	0.89	0.967	4.39	13.48	3.073	6.49	15.63	2.408	306.15	238.38	0.779	[EventTime]	same
q26	10.97	11.47	1.046	15.35	21.12	1.376	37.51	44.09	1.175	551.77	310.18	0.562	[SearchPhrase]	same
q27	0.90	0.81	0.898	4.39	19.15	4.359	6.32	21.00	3.321	288.57	258.05	0.894	[EventTime SearchPhrase]	same
q28	78.60	65.52	0.834	85.73	71.81	0.838	244.59	205.40	0.840	218.50	90.21	0.413	[]	same
q29	346.13	347.95	1.005	433.26	439.65	1.015	1129.15	1141.84	1.011	495.99	476.55	0.961	[]	same
q30	0.02	0.02	1.010	2.77	4.60	1.661	2.84	4.67	1.647	56.19	11.33	0.202	[ResolutionWidth]	same
q31	42.48	36.66	0.863	61.22	54.33	0.887	147.33	133.61	0.907	305.97	109.23	0.357	[]	same
q32	49.95	44.68	0.894	77.07	70.81	0.919	180.28	162.27	0.900	430.85	261.29	0.606	[]	same
q33	203.85	211.71	1.039	368.85	363.86	0.986	780.62	794.62	1.018	597.05	553.40	0.927	[]	same
q34	39.19	38.59	0.985	72.87	72.75	0.998	151.82	149.90	0.987	138.17	113.36	0.820	[]	same
q35	39.26	38.79	0.988	70.05	72.03	1.028	149.91	149.71	0.999	141.41	116.76	0.826	[]	same
q36	63.87	58.70	0.919	87.21	81.98	0.940	216.85	202.53	0.934	162.63	119.97	0.738	[]	same
q37	16.74	9.08	0.542	27.08	18.37	0.678	62.70	37.17	0.593	41.75	23.31	0.558	[]	same
q38	15.34	8.13	0.530	24.84	16.01	0.645	55.87	32.19	0.576	38.58	20.10	0.521	[]	same
q39	7.20	4.39	0.611	14.62	10.27	0.702	29.34	20.19	0.688	38.73	18.81	0.486	[]	same
q40	59.26	51.39	0.867	91.77	85.66	0.933	212.74	192.17	0.903	119.92	94.56	0.789	[]	same
q41	5.75	3.85	0.670	13.23	10.05	0.760	25.08	18.10	0.722	74.27	43.30	0.583	[]	same
q42	4.45	4.59	1.032	11.06	10.64	0.963	20.27	20.38	1.005	37.12	19.97	0.538	[]	same
q43	9.06	7.48	0.826	18.47	18.08	0.978	37.28	34.01	0.912	40.48	26.84	0.663	[]	same
totals over all 43: warm 1709.2 vs 1639.8 (0.959); first run 2524.0 vs 2570.1 (1.018); 3-run 5997.0 vs 5921.8 (0.987)
answer_mismatches none
order_differences none
intended_answer_changes none
scan_only(gated) n=30; gmeans over base>=1ms (per column): warm n=24 0.605, first n=25 0.713, cold n=25 0.354; worst warm=1.276
below_1ms_excluded_from_warm_gmean q1 q2 q4 q5 q6 q7
below_1ms_excluded_from_first_gmean q1 q4 q5 q6 q7
below_1ms_excluded_from_cold_gmean q1 q4 q5 q6 q7
mat rows missing none
scan_only_list_but_materializing none
measured_materializing q9(2 cols) q10(2 cols) q11(2 cols) q14(2 cols) q19(1 cols) q21(1 cols) q22(1 cols) q23(4 cols) q24(1 cols) q25(1 cols) q26(1 cols) q27(2 cols) q30(1 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q6(measured scan, list other) q12(measured scan, list other) q13(measured scan, list other) q15(measured scan, list other) q20(measured scan, list other) q28(measured scan, list other) q29(measured scan, list other) q31(measured scan, list other) q32(measured scan, list other) q36(measured scan, list other) q37(measured scan, list other) q38(measured scan, list other) q40(measured scan, list other) q41(measured scan, list other) q42(measured scan, list other) q43(measured scan, list other)
worst5 warm [(1.276, 'q6'), (1.272, 'q5'), (1.15, 'q7'), (1.064, 'q4'), (1.044, 'q13')]
worst5 cold [(1.877, 'q7'), (1.278, 'q4'), (1.14, 'q5'), (1.139, 'q6'), (1.044, 'q1')]
worst5 first [(1.802, 'q7'), (1.379, 'q4'), (1.144, 'q5'), (1.118, 'q6'), (1.028, 'q35')]
worst5 materializing_warm [(1.046, 'q26'), (1.036, 'q10'), (1.01, 'q30'), (1.005, 'q23'), (1.005, 'q14')]
first_run_total_ratio 1.018 (PI after P1: 2.99)
pi_row q41 warm 0.670 first 0.760 3run 0.722
pi_row q42 warm 1.032 first 0.963 3run 1.005

## arm B: parted p0 vs p2 (ms; ratio = p2/p0)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.109	0.01	0.01	1.622	0.01	0.01	1.587	0.01	0.01	0.887	[]	same
q2	6.56	6.33	0.965	17.30	16.31	0.943	30.73	29.19	0.950	47.35	16.15	0.341	[]	same
q3	539.64	0.00	0.000	557.61	0.05	0.000	1635.60	0.06	0.000	558.59	0.05	0.000	[]	same
q4	273.79	0.00	0.000	322.70	0.04	0.000	876.71	0.04	0.000	367.19	0.04	0.000	[]	same
q5	277.96	0.00	0.000	322.63	0.03	0.000	881.47	0.03	0.000	367.98	0.03	0.000	[]	same
q6	344.09	345.30	1.004	9719.47	9697.16	0.998	10417.07	10383.39	0.997	9830.42	9811.92	0.998	[SearchPhrase]	same
q7	532.80	0.00	0.000	561.08	0.05	0.000	1635.24	0.06	0.000	573.43	0.05	0.000	[]	same
q8	6.26	2.27	0.363	8.02	3.75	0.467	21.29	8.46	0.397	41.15	3.84	0.093	[]	same
q9	85.43	83.45	0.977	153.45	163.34	1.064	325.17	334.04	1.027	388.45	189.81	0.489	[RegionID UserID]	same
q10	78.66	79.19	1.007	155.40	170.54	1.097	313.19	335.31	1.071	439.11	196.38	0.447	[RegionID UserID]	same
q11	20.02	16.23	0.810	38.87	29.85	0.768	81.52	65.84	0.808	356.56	57.01	0.160	[]	same
q12	22.78	18.08	0.793	41.02	30.51	0.744	87.12	67.27	0.772	397.95	57.02	0.143	[]	same
q13	36.61	37.42	1.022	69.70	52.08	0.747	144.53	128.09	0.886	138.86	69.18	0.498	[]	same
q14	99.20	85.63	0.863	135.40	118.86	0.878	333.16	291.91	0.876	407.33	156.25	0.384	[]	same
q15	43.32	43.55	1.005	89.61	63.68	0.711	176.61	157.69	0.893	199.94	86.35	0.432	[]	same
q16	72.45	61.50	0.849	97.85	87.09	0.890	247.76	209.86	0.847	247.41	155.63	0.629	[]	same
q17	95.96	90.58	0.944	135.85	124.36	0.915	331.98	313.02	0.943	473.53	234.48	0.495	[]	same
q18	17.13	12.63	0.737	84.61	18.28	0.216	118.99	43.79	0.368	328.67	62.09	0.189	[]	same
q19	142.46	145.65	1.022	338.73	243.29	0.718	631.58	538.52	0.853	748.57	370.61	0.495	[EventTime]	same
q20	4.62	1.47	0.318	14.49	12.04	0.831	24.06	15.51	0.645	154.00	22.60	0.147	[]	same
q21	41.81	41.60	0.995	46.88	58.03	1.238	131.66	144.11	1.095	87.86	99.50	1.132	[URL]	same
q22	57.07	58.07	1.017	69.60	74.38	1.069	186.96	195.77	1.047	198.68	152.31	0.767	[URL]	same
q23	87.10	89.08	1.023	101.89	113.09	1.110	277.59	291.17	1.049	421.59	278.41	0.660	[Title URL]	same
q24	42.40	43.03	1.015	52.82	64.47	1.221	137.89	152.03	1.103	744.75	443.91	0.596	[URL]	same
q25	27.55	23.13	0.839	46.86	36.09	0.770	102.94	83.09	0.807	590.78	255.37	0.432	[]	same
q26	83.21	75.89	0.912	198.19	185.62	0.937	364.67	341.12	0.935	650.33	395.12	0.608	[]	same
q27	51.22	38.21	0.746	71.12	52.62	0.740	175.41	130.01	0.741	622.46	275.35	0.442	[]	same
q28	87.10	87.55	1.005	129.80	130.92	1.009	304.58	308.88	1.014	270.36	153.32	0.567	[]	same
q29	358.15	358.48	1.001	469.01	451.46	0.963	1189.11	1173.67	0.987	534.13	499.25	0.935	[]	same
q30	265.03	270.82	1.022	280.37	285.52	1.018	830.54	837.65	1.009	291.76	293.93	1.007	[ResolutionWidth]	same
q31	52.25	51.87	0.993	123.78	78.68	0.636	235.95	187.21	0.793	391.46	119.16	0.304	[]	same
q32	66.43	66.77	1.005	179.76	104.41	0.581	314.73	240.04	0.763	583.88	275.81	0.472	[]	same
q33	212.09	217.45	1.025	377.94	382.78	1.013	813.44	828.12	1.018	612.18	626.15	1.023	[]	same
q34	43.00	43.25	1.006	96.33	75.71	0.786	184.50	162.92	0.883	175.95	122.20	0.695	[]	same
q35	43.31	43.37	1.001	96.55	75.68	0.784	188.02	163.14	0.868	176.13	119.54	0.679	[]	same
q36	145.87	144.43	0.990	219.54	197.80	0.901	514.84	498.83	0.969	298.47	207.98	0.697	[]	same
q37	35.84	21.83	0.609	69.82	37.82	0.542	142.81	83.10	0.582	224.88	82.91	0.369	[]	same
q38	35.17	20.13	0.572	68.69	38.19	0.556	144.09	81.10	0.563	229.44	81.59	0.356	[]	same
q39	14.99	8.21	0.548	27.52	19.95	0.725	58.37	36.55	0.626	114.95	25.44	0.221	[]	same
q40	117.32	108.90	0.928	427.40	349.12	0.817	667.55	568.70	0.852	785.85	446.00	0.568	[]	same
q41	17.59	13.24	0.753	39.31	27.77	0.706	75.21	55.40	0.737	262.07	179.59	0.685	[]	same
q42	13.21	12.52	0.947	24.04	23.95	0.996	51.27	54.29	1.059	191.39	150.19	0.785	[]	same
q43	28.09	15.97	0.569	41.14	28.13	0.684	98.15	60.89	0.620	128.86	34.31	0.266	[]	same
totals over all 43: warm 4625.5 vs 2883.1 (0.623); first run 16122.1 vs 13723.5 (0.851); 3-run 25504.1 vs 19599.9 (0.768)
answer_mismatches none
order_differences none
intended_answer_changes none
scan_only(gated) n=34; gmeans over base>=1ms (per column): warm n=33 0.179, first n=33 0.251, cold n=33 0.146; worst warm=1.109
below_1ms_excluded_from_warm_gmean q1
below_1ms_excluded_from_first_gmean q1
below_1ms_excluded_from_cold_gmean q1
mat rows missing none
scan_only_list_but_materializing none
measured_materializing q6(1 cols) q9(2 cols) q10(2 cols) q19(1 cols) q21(1 cols) q22(1 cols) q23(2 cols) q24(1 cols) q30(1 cols)
class_vs_SCAN_ONLY_list_disagreements q5(measured scan, list other) q11(measured scan, list other) q12(measured scan, list other) q13(measured scan, list other) q14(measured scan, list other) q15(measured scan, list other) q20(measured scan, list other) q25(measured scan, list other) q26(measured scan, list other) q27(measured scan, list other) q28(measured scan, list other) q29(measured scan, list other) q31(measured scan, list other) q32(measured scan, list other) q36(measured scan, list other) q37(measured scan, list other) q38(measured scan, list other) q40(measured scan, list other) q41(measured scan, list other) q42(measured scan, list other) q43(measured scan, list other)
worst5 warm [(1.109, 'q1'), (1.025, 'q33'), (1.022, 'q13'), (1.006, 'q34'), (1.005, 'q32')]
worst5 cold [(1.023, 'q33'), (0.935, 'q29'), (0.887, 'q1'), (0.785, 'q42'), (0.697, 'q36')]
worst5 first [(1.622, 'q1'), (1.013, 'q33'), (1.009, 'q28'), (0.996, 'q42'), (0.963, 'q29')]
worst5 materializing_warm [(1.023, 'q23'), (1.022, 'q30'), (1.022, 'q19'), (1.017, 'q22'), (1.015, 'q24')]
pi_row q41 warm 0.753 first 0.706 3run 0.737
pi_row q42 warm 0.947 first 0.996 3run 1.059

## arm C0: splayed g0, base vs p2 binary (ms; ratio = p2/base)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.000	0.01	0.01	1.064	0.01	0.01	1.056	nan	nan	nan		same
q2	1.11	0.65	0.587	2.02	1.61	0.797	4.34	3.13	0.723	nan	nan	nan		same
q3	3.81	3.77	0.990	5.76	5.81	1.008	13.44	13.47	1.002	nan	nan	nan		same
q4	0.00	0.00	0.972	0.02	0.03	1.213	0.02	0.03	1.197	nan	nan	nan		same
q5	0.00	0.00	1.052	0.03	0.03	1.196	0.03	0.03	1.188	nan	nan	nan		same
q6	0.00	0.00	1.112	0.02	0.03	1.223	0.03	0.03	1.216	nan	nan	nan		same
q7	0.00	0.00	0.956	0.02	0.02	0.986	0.02	0.02	0.990	nan	nan	nan		same
q8	3.53	3.52	0.998	8.70	8.97	1.032	15.88	16.45	1.036	nan	nan	nan		same
q9	77.16	75.53	0.979	115.64	114.25	0.988	272.23	270.00	0.992	nan	nan	nan		same
q10	69.22	68.38	0.988	98.33	98.19	0.999	238.79	236.33	0.990	nan	nan	nan		same
q11	16.13	11.69	0.725	25.42	21.88	0.861	57.86	45.46	0.786	nan	nan	nan		same
q12	16.32	11.98	0.734	25.70	21.01	0.818	58.98	45.29	0.768	nan	nan	nan		same
q13	31.94	22.49	0.704	41.13	32.33	0.786	106.25	78.13	0.735	nan	nan	nan		same
q14	63.12	52.59	0.833	88.86	77.46	0.872	225.08	186.53	0.829	nan	nan	nan		same
q15	36.36	26.59	0.731	47.64	39.66	0.832	124.96	93.49	0.748	nan	nan	nan		same
q16	70.22	69.81	0.994	95.77	93.78	0.979	238.87	239.45	1.002	nan	nan	nan		same
q17	93.22	93.15	0.999	128.69	131.45	1.021	319.27	319.69	1.001	nan	nan	nan		same
q18	6.94	6.86	0.989	14.61	14.56	0.997	28.68	28.38	0.989	nan	nan	nan		same
q19	137.84	138.59	1.005	213.27	209.20	0.981	495.26	492.98	0.995	nan	nan	nan		same
q20	1.15	1.14	0.987	6.69	6.94	1.036	9.28	9.64	1.039	nan	nan	nan		same
q21	41.77	42.02	1.006	45.40	45.35	0.999	129.94	131.44	1.011	nan	nan	nan		same
q22	27.62	18.11	0.656	31.96	26.36	0.825	87.35	62.88	0.720	nan	nan	nan		same
q23	48.83	39.30	0.805	61.06	55.10	0.902	159.55	133.66	0.838	nan	nan	nan		same
q24	29.15	29.47	1.011	34.48	34.85	1.011	94.62	94.03	0.994	nan	nan	nan		same
q25	0.92	0.97	1.055	4.57	4.52	0.990	6.52	6.50	0.998	nan	nan	nan		same
q26	10.77	11.01	1.023	15.50	15.71	1.013	37.08	38.09	1.027	nan	nan	nan		same
q27	1.00	0.90	0.909	4.57	4.52	0.990	6.61	6.51	0.985	nan	nan	nan		same
q28	82.78	79.01	0.954	85.12	85.90	1.009	251.27	247.79	0.986	nan	nan	nan		same
q29	353.68	345.54	0.977	446.94	428.79	0.959	1161.71	1124.73	0.968	nan	nan	nan		same
q30	0.02	0.02	0.997	3.11	3.06	0.982	3.18	3.12	0.983	nan	nan	nan		same
q31	52.98	42.67	0.805	71.16	60.67	0.853	180.84	147.82	0.817	nan	nan	nan		same
q32	59.16	49.74	0.841	85.05	77.30	0.909	205.11	177.60	0.866	nan	nan	nan		same
q33	205.32	204.87	0.998	375.20	368.53	0.982	796.03	785.04	0.986	nan	nan	nan		same
q34	39.27	39.59	1.008	71.00	71.10	1.001	150.92	150.68	0.998	nan	nan	nan		same
q35	39.54	39.13	0.990	70.33	70.62	1.004	151.55	149.97	0.990	nan	nan	nan		same
q36	64.38	63.84	0.992	89.91	88.28	0.982	221.30	218.77	0.989	nan	nan	nan		same
q37	17.17	16.81	0.979	27.87	27.22	0.977	63.21	61.13	0.967	nan	nan	nan		same
q38	15.71	15.25	0.971	25.28	24.41	0.966	57.17	55.44	0.970	nan	nan	nan		same
q39	7.43	7.18	0.967	14.82	14.58	0.984	30.02	29.46	0.981	nan	nan	nan		same
q40	59.29	59.54	1.004	90.88	94.53	1.040	211.38	213.99	1.012	nan	nan	nan		same
q41	5.89	5.80	0.984	13.43	13.54	1.008	25.34	25.35	1.000	nan	nan	nan		same
q42	4.61	4.63	1.005	11.26	11.35	1.008	20.72	20.63	0.995	nan	nan	nan		same
q43	9.22	8.99	0.975	18.53	18.58	1.003	37.99	37.64	0.991	nan	nan	nan		same
totals over all 43: warm 1804.5 vs 1711.1 (0.948); first run 2615.8 vs 2522.1 (0.964); 3-run 6298.7 vs 6000.8 (0.953)
answer_mismatches none
order_differences none
intended_answer_changes none
gen0 warm_gmean(base>=1ms n=35)=0.911 warm_worst=1.112 worst5 [(1.112, 'q6'), (1.055, 'q25'), (1.052, 'q5'), (1.023, 'q26'), (1.011, 'q24')]

## arm C1: parted p0, base vs p2 binary (ms; ratio = p2/base)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	1.005	0.01	0.01	1.624	0.01	0.01	1.580	nan	nan	nan		same
q2	6.59	6.62	1.004	17.07	17.00	0.996	30.50	30.57	1.002	nan	nan	nan		same
q3	538.65	542.34	1.007	552.30	557.85	1.010	1637.62	1654.76	1.010	nan	nan	nan		same
q4	277.38	276.29	0.996	326.88	322.29	0.986	883.28	879.20	0.995	nan	nan	nan		same
q5	277.60	277.12	0.998	326.63	322.79	0.988	887.78	887.77	1.000	nan	nan	nan		same
q6	345.16	345.60	1.001	9712.81	9703.85	0.999	10414.46	10403.09	0.999	nan	nan	nan		same
q7	528.54	529.54	1.002	559.51	558.87	0.999	1622.71	1621.38	0.999	nan	nan	nan		same
q8	9.16	6.35	0.693	21.43	7.93	0.370	40.21	21.05	0.523	nan	nan	nan		same
q9	89.53	85.39	0.954	176.46	157.79	0.894	359.74	329.46	0.916	nan	nan	nan		same
q10	87.30	78.74	0.902	204.07	154.26	0.756	381.29	315.05	0.826	nan	nan	nan		same
q11	359.48	20.11	0.056	582.61	38.06	0.065	1304.41	80.70	0.062	nan	nan	nan		same
q12	359.00	22.59	0.063	582.23	40.76	0.070	1308.02	86.39	0.066	nan	nan	nan		same
q13	40.40	36.86	0.912	70.10	68.87	0.982	152.48	143.36	0.940	nan	nan	nan		same
q14	1136.43	99.48	0.088	1416.70	133.82	0.094	3698.21	337.26	0.091	nan	nan	nan		same
q15	46.81	43.10	0.921	89.25	89.10	0.998	183.80	177.08	0.963	nan	nan	nan		same
q16	76.16	72.50	0.952	147.82	97.91	0.662	301.59	248.74	0.825	nan	nan	nan		same
q17	103.27	95.81	0.928	202.51	134.00	0.662	414.22	331.84	0.801	nan	nan	nan		same
q18	17.28	17.06	0.987	84.00	85.19	1.014	119.62	120.18	1.005	nan	nan	nan		same
q19	142.40	143.57	1.008	336.35	334.79	0.995	621.44	627.00	1.009	nan	nan	nan		same
q20	4.61	4.65	1.008	14.26	14.47	1.014	23.77	24.31	1.023	nan	nan	nan		same
q21	41.72	41.64	0.998	46.42	46.74	1.007	132.70	130.35	0.982	nan	nan	nan		same
q22	60.87	57.11	0.938	85.63	69.42	0.811	211.01	184.88	0.876	nan	nan	nan		same
q23	93.99	87.09	0.927	207.50	101.24	0.488	398.34	281.75	0.707	nan	nan	nan		same
q24	44.47	42.69	0.960	129.93	52.33	0.403	220.39	138.56	0.629	nan	nan	nan		same
q25	1070.99	27.74	0.026	1333.11	46.08	0.035	3488.02	101.91	0.029	nan	nan	nan		same
q26	1126.15	83.47	0.074	1482.85	194.96	0.131	3745.00	367.20	0.098	nan	nan	nan		same
q27	1087.51	51.31	0.047	1344.53	69.55	0.052	3529.24	173.56	0.049	nan	nan	nan		same
q28	91.96	87.14	0.948	146.74	125.34	0.854	334.21	301.75	0.903	nan	nan	nan		same
q29	362.84	356.48	0.982	470.21	462.92	0.985	1195.05	1182.88	0.990	nan	nan	nan		same
q30	265.57	269.91	1.016	284.15	279.79	0.985	837.26	833.48	0.995	nan	nan	nan		same
q31	55.32	51.85	0.937	123.36	122.04	0.989	235.71	235.07	0.997	nan	nan	nan		same
q32	69.48	65.06	0.936	178.33	173.42	0.972	318.51	307.42	0.965	nan	nan	nan		same
q33	217.46	212.79	0.979	469.99	371.52	0.790	912.64	804.87	0.882	nan	nan	nan		same
q34	43.38	42.78	0.986	94.96	94.72	0.997	182.99	182.24	0.996	nan	nan	nan		same
q35	388.21	42.85	0.110	2295.85	94.32	0.041	3075.02	180.85	0.059	nan	nan	nan		same
q36	146.90	145.15	0.988	218.07	214.93	0.986	516.83	515.32	0.997	nan	nan	nan		same
q37	36.52	35.89	0.983	70.44	68.74	0.976	145.68	142.61	0.979	nan	nan	nan		same
q38	36.52	34.88	0.955	68.50	67.30	0.982	147.56	144.83	0.981	nan	nan	nan		same
q39	15.41	15.15	0.983	27.66	27.27	0.986	59.30	58.04	0.979	nan	nan	nan		same
q40	117.49	118.17	1.006	420.16	420.96	1.002	658.41	663.71	1.008	nan	nan	nan		DIFF
q41	17.74	17.54	0.989	39.32	38.95	0.991	74.97	75.17	1.003	nan	nan	nan		same
q42	13.49	13.18	0.977	24.22	23.92	0.988	51.56	50.77	0.985	nan	nan	nan		same
q43	28.28	28.38	1.003	41.51	40.80	0.983	100.29	97.98	0.977	nan	nan	nan		same
totals over all 43: warm 9878.0 vs 4631.9 (0.469); first run 25026.4 vs 16046.9 (0.641); 3-run 44955.8 vs 25474.4 (0.567)
answer_mismatches q40:DIFF
order_differences none
intended_answer_changes q40:DIFF
gen0 warm_gmean(base>=1ms n=42)=0.608 warm_worst=1.016 worst5 [(1.016, 'q30'), (1.008, 'q20'), (1.008, 'q19'), (1.007, 'q3'), (1.006, 'q40')]
q16 first run base 147.8 ms, p2 97.9 ms (carry 2)

## arm R: parted ragged, base vs p2 binary (ms; ratio = p2/base)
q	warm_a	warm_b	wr	first_a	first_b	fr	3run_a	3run_b	tr	cold_a	cold_b	cr	materialized	answers
q1	0.00	0.00	0.935	0.01	0.01	0.563	0.01	0.01	0.583	nan	nan	nan		same
q2	7.74	7.65	0.989	18.33	16.93	0.924	33.99	32.32	0.951	nan	nan	nan		same
q3	582.03	272.86	0.469	596.05	285.24	0.479	1762.42	835.76	0.474	nan	nan	nan		same
q4	277.78	276.77	0.996	330.84	322.07	0.973	889.46	874.50	0.983	nan	nan	nan		same
q5	277.79	275.52	0.992	328.25	321.62	0.980	892.00	881.60	0.988	nan	nan	nan		same
q6	344.55	345.21	1.002	9731.16	9710.51	0.998	10433.91	10396.20	0.996	nan	nan	nan		same
q7	526.38	533.66	1.014	550.69	553.43	1.005	1602.74	1628.13	1.016	nan	nan	nan		same
q8	9.63	8.77	0.911	22.58	10.39	0.460	42.01	28.10	0.669	nan	nan	nan		same
q9	89.17	85.48	0.959	179.58	152.19	0.847	359.37	328.28	0.913	nan	nan	nan		same
q10	87.47	78.77	0.901	206.06	157.82	0.766	383.44	323.65	0.844	nan	nan	nan		same
q11	375.70	20.54	0.055	655.17	36.52	0.056	1408.92	78.53	0.056	nan	nan	nan		same
q12	375.79	22.63	0.060	654.67	39.85	0.061	1407.74	85.32	0.061	nan	nan	nan		same
q13	40.14	36.84	0.918	69.80	68.76	0.985	151.00	143.80	0.952	nan	nan	nan		same
q14	1186.41	99.39	0.084	1513.88	132.47	0.088	3902.02	339.35	0.087	nan	nan	nan		same
q15	46.28	42.85	0.926	89.70	88.20	0.983	187.51	175.44	0.936	nan	nan	nan		same
q16	76.07	72.92	0.959	145.31	101.63	0.699	301.77	251.53	0.834	nan	nan	nan		same
q17	103.42	95.92	0.927	204.11	134.32	0.658	415.73	331.83	0.798	nan	nan	nan		same
q18	17.13	17.09	0.997	84.07	83.32	0.991	120.12	117.90	0.981	nan	nan	nan		same
q19	142.13	142.25	1.001	338.99	341.04	1.006	625.99	628.26	1.004	nan	nan	nan		same
q20	4.45	4.46	1.001	14.09	14.29	1.014	23.42	23.80	1.016	nan	nan	nan		same
q21	42.15	41.45	0.983	46.42	46.77	1.008	130.83	130.66	0.999	nan	nan	nan		same
q22	60.42	57.13	0.946	85.23	69.39	0.814	206.11	184.48	0.895	nan	nan	nan		same
q23	94.21	86.97	0.923	270.38	101.88	0.377	465.53	278.10	0.597	nan	nan	nan		same
q24	44.51	42.40	0.953	195.83	51.60	0.263	290.52	136.56	0.470	nan	nan	nan		same
q25	1123.32	27.67	0.025	1434.39	44.66	0.031	3700.96	100.68	0.027	nan	nan	nan		same
q26	1178.61	82.82	0.070	1581.32	192.50	0.122	3958.03	361.71	0.091	nan	nan	nan		same
q27	1138.23	51.37	0.045	1452.53	71.50	0.049	3731.13	174.25	0.047	nan	nan	nan		same
q28	91.75	86.96	0.948	148.30	129.24	0.871	333.29	307.74	0.923	nan	nan	nan		same
q29	363.98	355.23	0.976	475.01	469.05	0.987	1209.00	1189.22	0.984	nan	nan	nan		same
q30	266.57	269.38	1.011	286.25	279.32	0.976	834.06	834.90	1.001	nan	nan	nan		same
q31	55.16	51.78	0.939	124.94	123.81	0.991	237.81	235.07	0.988	nan	nan	nan		same
q32	69.20	65.76	0.950	179.09	176.36	0.985	317.38	309.29	0.975	nan	nan	nan		same
q33	218.12	213.78	0.980	464.00	375.98	0.810	904.83	816.11	0.902	nan	nan	nan		same
q34	43.45	43.14	0.993	98.20	96.60	0.984	187.59	185.15	0.987	nan	nan	nan		same
q35	412.87	43.08	0.104	2420.10	95.56	0.039	3251.32	182.59	0.056	nan	nan	nan		same
q36	148.29	145.23	0.979	219.61	218.02	0.993	524.05	514.07	0.981	nan	nan	nan		same
q37	36.48	33.12	0.908	72.60	66.35	0.914	148.54	134.30	0.904	nan	nan	nan		same
q38	35.56	32.51	0.914	72.82	66.91	0.919	146.83	138.21	0.941	nan	nan	nan		same
q39	15.14	12.41	0.820	28.95	24.56	0.848	59.73	49.43	0.828	nan	nan	nan		same
q40	151.92	148.38	0.977	462.70	447.32	0.967	769.19	751.87	0.977	nan	nan	nan		DIFF
q41	17.60	17.12	0.973	40.20	38.47	0.957	75.66	72.88	0.963	nan	nan	nan		same
q42	13.31	12.45	0.935	26.42	22.84	0.864	53.17	48.16	0.906	nan	nan	nan		same
q43	28.29	26.01	0.920	42.24	39.01	0.924	98.60	91.50	0.928	nan	nan	nan		same
totals over all 43: warm 10219.2 vs 4385.7 (0.429); first run 25960.9 vs 15818.3 (0.609); 3-run 46577.8 vs 24731.2 (0.531)
answer_mismatches q40:DIFF
order_differences none
intended_answer_changes q40:DIFF
gen0 warm_gmean(base>=1ms n=42)=0.589 warm_worst=1.014 worst5 [(1.014, 'q7'), (1.011, 'q30'), (1.002, 'q6'), (1.001, 'q20'), (1.001, 'q19')]

enc A: columns with DELTA vectors: ['UserID {raw:76 const:3 delta_bp:97 rle:9526 dict:64}]
enc B: columns with DELTA vectors: ['UserID {raw:75 const:3 delta_bp:89 rle:8963 dict:640}]
sizes {('A', 'g0'): 5948677939, ('A', 'g2'): 3307128731, ('B', 'p0'): 4807844987, ('B', 'p2'): 2072366907}  peak_rss_kb {('A', 'g0'): 5108136, ('A', 'g2'): 5054232, ('B', 'p0'): 12164164, ('B', 'p2'): 11963740}  evict_modes fadvise  ragged_partition ['9', '40']  meta {'p2': ['2f949edc', 'f98fef6700b4'], 'base': ['80a28e49', 'f1075b366780']}
GATE (strict) splayed[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=PASS] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (P3-exempt): FAIL
P3-EXEMPT LIST q8/A/warm[run-level compute (group over RLE runs)]: 3.58 -> 2.92 ms (x0.817) within-bound; q8/A/first[run-level compute (group over RLE runs, first run)]: 9.08 -> 8.30 ms (x0.913) within-bound; q18/A/first[row-list gather (first-n keys published, P3 path 2)]: 13.49 -> 5.92 ms (x0.439) within-bound; q39/A/first[selection gather (P3 path 2)]: 14.62 -> 10.27 ms (x0.702) within-bound; q39/B/first[selection gather (P3 path 2)]: 27.52 -> 19.95 ms (x0.725) within-bound; q39/A/cold[selection gather reads whole columns cold (P3)]: 38.73 -> 18.81 ms (x0.486) within-bound; q42/A/warm[point gathers of the selected keys (P3)]: 4.45 -> 4.59 ms (x1.032) within-bound; q42/B/warm[point gathers of the selected keys (P3)]: 13.21 -> 12.52 ms (x0.947) within-bound; q24/A/rss[top-k output gather RSS]: arm peak-RSS 5108136 -> 5054232 kB (x0.99, max 2.37) not gated; q25/A/rss[top-k output gather RSS]: arm peak-RSS 5108136 -> 5054232 kB (x0.99, max 2.37) not gated; q26/A/rss[top-k output gather RSS]: arm peak-RSS 5108136 -> 5054232 kB (x0.99, max 2.37) not gated; q27/A/rss[top-k output gather RSS]: arm peak-RSS 5108136 -> 5054232 kB (x0.99, max 2.37) not gated
p3_unexpected_materializations none
rss A: 5108136 -> 5054232 kB (x0.989, bound 1.1)
rss B: 12164164 -> 11963740 kB (x0.984, bound 1.1)
carry q2/A/warm: P2 x1.086 -> P3 x0.617
carry q8/A/warm: P2 x1.424 -> P3 x0.817
carry q8/B/warm: P2 x1.060 -> P3 x0.363
carry q18/A/first: P2 x1.983 -> P3 x0.439
carry q18/B/warm: P2 x1.088 -> P3 x0.737
carry q39/A/first: P2 x2.402 -> P3 x0.702
carry q39/B/first: P2 x1.503 -> P3 x0.725
carry q42/A/warm: P2 x1.664 -> P3 x1.032
carry q42/B/warm: P2 x1.237 -> P3 x0.947
P3GATE p2_strict=FAIL materialization=PASS rss=PASS
P3 GATE: FAIL
```


## P4 index region (10M store, release build, commit 3abe880e)

The index-query driver is `W=/home/hetoku/data/tmp/p4_bench
p4_bench.sh all` (prep 15:49-15:52, measured 15:52-15:58; a second
`prep clean` at 15:58-16:00 recorded the g2 size the first prep missed, with
the same binary).  The binary is a2ca3564's release build (sha256
a8b202bb00e0; 3abe880e adds only the scripts).  Both copies come from the
`.db.splayed.get` store /skull/clickbench/rfsplayed with explicit HASH
indexes on UserID, RefererHash and URLHash (`update` keeps the index: the
source, g0 and g2 report `hash` on all three).  9 rounds alternated
(order swapped per round), 6 timed runs per process; first touch is a fresh
process with the store's files evicted (3 trials).  The load average was
checked before each round; one wait (40 s, 15:54) before the first first-touch
trial.  Raw data: /home/hetoku/data/tmp/p4_bench/p4.tsv, summary.txt beside it.

Query shapes: a count over a select is answered before the index arm, so
the lookups are projecting selects.  q1 is ClickBench q20 as written; its key
is absent from the 10M store (0 rows, the HASH proves it absent).  `.idx.uses`
shows the same index consulted once on g0 and g2 for every query (D2); g2
leaves only the looked-up column materialized.

| query | warm g0 ms | warm g2 ms | ratio | best round g0 / g2 (ratio) | in-process run 1 g0 / g2 | first touch g0 / g2 ms | rows | answers warm / first |
|---|---|---|---|---|---|---|---|---|
| q1 UserID point lookup (ClickBench q20) | 0.009 | 0.008 | 0.895 | 0.008 / 0.008 (0.963) | 0.764 / 253.282 | 1.4 / 630.4 | 0 | same / same |
| q2 RefererHash point lookup (q41's constant) | 0.358 | 0.310 | 0.867 | 0.326 / 0.279 (0.855) | 1.396 / 636.295 | 15.2 / 1312.1 | 81558 | same / same |
| q3 URLHash point lookup (q42's constant) | 0.371 | 0.352 | 0.949 | 0.358 / 0.295 (0.824) | 1.425 / 653.018 | 15.4 / 1253.0 | 90899 | same / same |
| q4 ClickBench q41 | 5.832 | 4.888 | 0.838 | 5.713 / 4.646 (0.813) | 13.318 / 644.923 | 98.9 / 1347.8 | 10 | same / same |
| q5 ClickBench q42 | 4.656 | 4.680 | 1.005 | 4.560 / 4.595 (1.008) | 10.961 / 659.465 | 37.2 / 1270.5 | 10 | same / same |

rounds: 9; first-touch trials: 3
sizes: g0 6086708243 B, g2 3354488811 B (x0.551)
index kinds (UserID RefererHash URLHash): source ['hash 'hash 'hash]; g0 ['hash 'hash 'hash]; g2 ['hash 'hash 'hash]
uses q1: g0 {UserID:1}	[] | g2 {UserID:1}	[UserID]
uses q2: g0 {RefererHash:1}	[] | g2 {RefererHash:1}	[RefererHash]
uses q3: g0 {URLHash:1}	[] | g2 {URLHash:1}	[URLHash]
uses q4: g0 {RefererHash:1}	[] | g2 {RefererHash:1}	[RefererHash]
uses q5: g0 {URLHash:1}	[] | g2 {URLHash:1}	[URLHash]
meta p4=a2ca3564/a8b202bb00e0
load waits first1/15:54:20/40

**P4 index answers: PASS** (every query's full answer, warm and first touch, g2 equal to g0)
**P4 index queries warm: PASS** (each warm median at most 1.05x generation 0, or under 1 ms within 0.05 ms)

- Equivalence pass (`make test TEST_FILTER=equiv/`): 1178 tests, 0 failed, 0
  runtime errors, at the default cores and at 8.  The indexed saving `.rfl`
  files it runs: 21.  The brief's grep lists 22, but
  `hof/eval_coverage3.rfl` matches only a comment and saves nothing, so the
  pass does not run it.  The list includes `storage/compress_index.rfl`.
  No triage was needed: no failure, no new D1 site, no `equiv_skip` entry,
  `equiv_groups` unchanged.
- Index validation suite (`make test TEST_FILTER=idxval/`): 65 tests, 0
  failed at cores 0, 2 and 8.  `srccheck/` 11/11.  `enccorrupt/` 16/16 at
  cores 2 and 8.  `fuzz_flips` gains an I64 HASH column saved by
  `ray_col_save_gen` (a2ca3564): 300 of 300 flips confined to its index
  region were rejected, and structural flips over all six bases were rejected
  1184 of 1200 times.  Full suite: 5920/5920, 0 runtime errors.
- WASM golden run: not run (no emcc on this machine); wasm32 layout check PASS — SPEC DEVIATION.
- First touch (fresh process, cache evicted per file): generation 2 pays the
  column decode and the region parse.  This is reported, not gated (spec
  "Benchmarks").  About 630 ms for UserID and 1.25-1.35 s for the
  RefererHash and URLHash columns, against 1-100 ms on generation 0.
  In-process run 1 (warm page cache) is 250-660 ms.  A profile of q41's first
  use attributes 47% to `ray_enc_index_validate` (32% in its sort for
  distinctness) and 7% to the decode.

**P4 gate (equivalence): PASS**
**P4 gate (index validation suite): PASS**
**P4 index answers: PASS** (every query's full answer, warm and first touch, g2 equal to g0)
**P4 index queries warm: PASS** (each warm median at most 1.05x generation 0, or under 1 ms within 0.05 ms)

### D7: the P2/P3 driver rerun (no regression, I8)

`W=/home/hetoku/data/tmp/p2_bench BASE=25eabaf2 p2_bench.sh prep`, then
`splayed` (16:04-16:35) and `parted` (16:35-17:36).  The P4 binary is
3abe880e (release, sha256 a2d3151c0de3, same source as a2ca3564).  The base
is 25eabaf2, the end of P3 (1e9feb84f335).  The driver's previous data in
that directory (P2 gate, 2026-10-10) was moved to `old_p2gate_20261010/`.
Load was 1.00 at the start; no other heavy job ran (a light cron worker
was the only other process seen).  `p3_summary.py` is unchanged, including
its ALLOW table.  Raw data: /home/hetoku/data/tmp/p2_bench/p2.tsv and
summary.txt.

```
totals over all 43: warm 1707.3 vs 1647.7 (0.965); first run 2516.1 vs 3882.2 (1.543); 3-run 5989.7 vs 7241.1 (1.209)
answer_mismatches none
first_run_total_ratio 1.543 (PI after P1: 2.99)
pi_row q41 warm 0.814 first 47.001 3run 25.474
pi_row q42 warm 1.005 first 58.635 3run 32.166
totals over all 43: warm 4707.3 vs 2904.4 (0.617); first run 16199.4 vs 13738.1 (0.848); 3-run 25750.6 vs 19671.9 (0.764)
answer_mismatches none
pi_row q41 warm 0.792 first 0.688 3run 0.751
pi_row q42 warm 0.954 first 0.997 3run 1.068
totals over all 43: warm 1711.1 vs 1711.1 (1.000); first run 2524.3 vs 2531.3 (1.003); 3-run 6011.3 vs 6017.0 (1.001)
answer_mismatches none
gen0 warm_gmean(base>=1ms n=34)=0.998 warm_worst=1.102 worst5 [(1.102, 'q1'), (1.089, 'q25'), (1.057, 'q7'), (1.022, 'q42'), (1.021, 'q5')]
totals over all 43: warm 4650.9 vs 4713.8 (1.014); first run 16076.3 vs 16160.0 (1.005); 3-run 25486.7 vs 25750.1 (1.010)
answer_mismatches none
gen0 warm_gmean(base>=1ms n=42)=1.005 warm_worst=1.046 worst5 [(1.046, 'q5'), (1.045, 'q4'), (1.04, 'q26'), (1.028, 'q7'), (1.026, 'q6')]
totals over all 43: warm 4392.1 vs 4459.9 (1.015); first run 15853.2 vs 15930.9 (1.005); 3-run 24764.8 vs 24998.8 (1.009)
answer_mismatches none
gen0 warm_gmean(base>=1ms n=42)=1.006 warm_worst=1.052 worst5 [(1.052, 'q5'), (1.049, 'q4'), (1.04, 'q30'), (1.036, 'q26'), (1.027, 'q6')]
sizes {('A', 'g0'): 5948677939, ('A', 'g2'): 3216455899, ('B', 'p0'): 4807844987, ('B', 'p2'): 2072366907}  peak_rss_kb {('A', 'g0'): 5118136, ('A', 'g2'): 5970564, ('B', 'p0'): 12144580, ('B', 'p2'): 11965660}  evict_modes fadvise  ragged_partition ['9', '40']  meta {'p2': ['3abe880e', 'a2d3151c0de3'], 'base': ['25eabaf2', '1e9feb84f335']}
GATE (strict) splayed[answers=PASS warm=FAIL cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=FAIL cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (P3-exempt): FAIL
p3_unexpected_materializations A/q41:RefererHash A/q42:URLHash
rss A: 5118136 -> 5970564 kB (x1.167, bound 1.1)
rss B: 12144580 -> 11965660 kB (x0.985, bound 1.1)
carry q2/A/warm: P2 x1.086 -> P3 x0.617
carry q8/A/warm: P2 x1.424 -> P3 x0.831
carry q8/B/warm: P2 x1.060 -> P3 x0.362
carry q18/A/first: P2 x1.983 -> P3 x0.435
carry q18/B/warm: P2 x1.088 -> P3 x0.736
carry q39/A/first: P2 x2.402 -> P3 x0.690
carry q39/B/first: P2 x1.503 -> P3 x0.735
carry q42/A/warm: P2 x1.664 -> P3 x1.005
carry q42/B/warm: P2 x1.237 -> P3 x0.954
P3GATE p2_strict=FAIL materialization=FAIL rss=FAIL
P3 GATE: FAIL
```

**D7 no-regression rerun: FAIL by the letter, after Task 10b; every failing clause is ruled** (Rulings 10-1 to 10-7).  Materialization and RSS PASS (Ruling 10-1); first index use fixed (Ruling 10-2; q2/q3 61-63 ms accepted, Ruling 10-6); splayed warm q33 1.068 accepted (Ruling 10-7, post-spec tuning); gen-0 C0 q25 1.111, q27 1.077 and gmean 1.001 (Ruling 10-4 extended) and gen-0 parted (Ruling 10-5) accepted.  Answers PASS on every arm.  The rerun is in "Task 10b" below.

**P4 gate: PASS** (Rulings 10-1 to 10-7)

Failing clauses, each profiled on the release binaries (`perf record -g`;
the top-5 tables are in the Task 10 report):

1. **Materialization, A/q41 RefererHash, A/q42 URLHash**, and **RSS A x1.167**.
   The source store carries a persisted HASH on RefererHash, URLHash and
   WatchID.  Up to P3, generation 2 wrote these indexed columns as
   generation 0: a base-written g2 reports gen-0 HASH dicts on them.  Under P4
   they are generation 2 with an index region.  D2 sends q41 / q42 down
   generation 0's index route: materialize, then probe the HASH.  The
   counters match gen 0 (`.idx.uses` {RefererHash:1} / {URLHash:1} on both).
   The warm times are 0.814 and 1.005.  The first runs are 647 / 657 ms
   against 14 / 11 ms on g0 (47x / 59x; PI rows above).  The RSS excess is
   entirely q41 / q42: a 41-query run without them gives g0 5 080 248 kB
   and g2 5 059 724 kB (x0.996).  With all 43, the figures are 5 104 436 and
   5 965 972 kB.  This is D2's stated cost ("one materialization per indexed
   column"), not an answer change.  The ALLOW table and the RSS bound
   predate indexed generation-2 columns.
2. **Splayed warm (A), q33 1.082** (202.6 -> 219.3 ms; the P3 rerun gave 1.039).
   The group runs on the P3 fused encoded path on g2:
   `agg_radix_group_fused_fn`, `agg_radix_fused_fn` and
   `agg_radix_stage_block_c`, against `agg_radix_group_fn` and
   `agg_radix_scatter_fn` on g0.  The key WatchID (10M distinct, HASH) is
   now generation 2: enc raw:9702 dict:64.  On P3 it was a generation-0 file.
   On the same P4 binary, a P3-written g2 (WatchID gen 0) takes 213 ms and a
   P4-written g2 takes 217.5 ms; the base binary on the P3-written g2 takes
   213 ms.  About 2 points come from WatchID being encoded, and about 4 points
   are P3's existing gap.  Nothing in P4's code path runs here: no
   materialization, no index use.
3. **Gen-0 splayed warm (C0), q25 1.089** (0.93 -> 1.01 ms, a gap of 0.08 ms
   over the 0.05 ms floor).  This is Ruling 14-8's class.  A 4-pass recheck
   of 41 runs gives medians of base 0.96-1.01 and p4 1.00-1.01, with equal
   minimums.  3000 in-process iterations take 2947 vs 2957 ms (1.003), with
   identical profiles (`fpk_par_fn_plain`, `fpk_cmp`, `fp_eval_cmp`).
4. **Gen-0 parted warm (C1 gmean 1.005; R gmean 1.006 and q5 1.052)**.
   The worst queries are q4 `(avg (at hits 'UserID))` and q5
   `(count (distinct (at hits 'UserID)))`, at 278 -> 291 ms, plus q6, q7 and
   q26 at +3-4%.  The cost is parted `at`, the per-element
   `parted_to_flat_vec` (`ray_alloc` / `ray_free` / `store_typed_elem`),
   with identical profiles on both binaries.  `perf stat` on q4 gives
   46.57 G instructions on both (within 0.003%) but cycles +3.9%.  None
   of these functions changed in P4: `git diff 25eabaf2` touches no file
   under src/lang or src/mem, nor src/ops/collection.c (`parted_to_flat_vec`)
   or src/store/part.c (the splay.c change is write-side).  The cause is code or data
   layout, not P4 logic.

### Task 10b: first index use on generation 2, the D7 rerun (Rulings 10-1 to 10-5)

The index-query driver was rerun in full on 5b8a8ff6 (release, sha256
af0c122eecba): `W=/home/hetoku/data/tmp/p4_bench p4_bench.sh clean all`
(20:11-20:18, 9 rounds, 3 first-touch trials; two 20 s load waits).
ff68a997 changes the validator and the parse only (the stores the writer
produces are the same).

| query | warm g0 ms | warm g2 ms | ratio | best round g0 / g2 (ratio) | in-process run 1 g0 / g2 | first touch g0 / g2 ms | rows | answers warm / first |
|---|---|---|---|---|---|---|---|---|
| q1 UserID point lookup (ClickBench q20) | 0.009 | 0.009 | 0.966 | 0.008 / 0.008 (0.947) | 0.740 / 45.347 | 1.3 / 293.2 | 0 | same / same |
| q2 RefererHash point lookup (q41's constant) | 0.342 | 0.327 | 0.957 | 0.318 / 0.263 (0.826) | 1.447 / 61.182 | 15.3 / 523.1 | 81558 | same / same |
| q3 URLHash point lookup (q42's constant) | 0.388 | 0.342 | 0.881 | 0.345 / 0.295 (0.856) | 1.441 / 63.202 | 15.3 / 494.6 | 90899 | same / same |
| q4 ClickBench q41 | 5.847 | 4.717 | 0.807 | 5.698 / 4.568 (0.802) | 13.276 / 70.473 | 74.7 / 553.7 | 10 | same / same |
| q5 ClickBench q42 | 4.615 | 4.677 | 1.013 | 4.505 / 4.566 (1.014) | 11.119 / 71.871 | 36.7 / 509.1 | 10 | same / same |

meta p4=5b8a8ff6/af0c122eecba; sizes, kinds and `.idx.uses` as above.

**P4 index answers: PASS** (every query's full answer, warm and first touch, g2 equal to g0)
**P4 index queries warm: PASS** (each warm median at most 1.05x generation 0, or under 1 ms within 0.05 ms)

First index use (Ruling 10-2), in-process run 1 on g2: q1 253 -> 45 ms,
q2 636 -> 61 ms, q3 653 -> 63 ms; first touch 630 -> 293, 1312 -> 523 and
1253 -> 495 ms.  The distinctness check moved into the reachability walk
(a sort only past a compare budget), and tables of 2^17 buckets or more are
checked and copied over the pool (the serial checks rerun on any doubt, so
verdicts and messages do not depend on the core count).  The rest of a
first use is mostly the page faults of about 270 MB of fresh heap (the
decoded column and the four HASH children) on this 4-core machine.

D7, the splayed stage only, on 5b8a8ff6 against BASE 25eabaf2:
`W=/home/hetoku/data/tmp/p2_bench BASE=25eabaf2 p2_bench.sh prep`, then
`splayed` (20:19-20:52).  Task 10's data in that directory was moved, not
deleted, to `old_p4t10_20261015/`.  The driver now records the memx rows
(arm A without queries 41 and 42, 5b8a8ff6).  Splayed-only summary:
/home/hetoku/data/tmp/p2_bench/summary.txt.  The gate lines below come from
the splayed rows joined with Task 10's parted rows (arms B, C1 and R,
binary 3abe880e; ff68a997 does not run on those arms, which hold no index
region): /home/hetoku/data/tmp/p4t10b/d7_merged.tsv.

```
totals over all 43: warm 1709.4 vs 1645.7 (0.963); first run 2522.3 vs 2705.8 (1.073); 3-run 5994.6 vs 6079.5 (1.014)
pi_row q41 warm 0.825 first 5.642 3run 3.388
pi_row q42 warm 1.036 first 6.638 3run 4.102
gen0 warm_gmean(base>=1ms n=34)=1.001 warm_worst=1.111 worst5 [(1.111, 'q25'), (1.077, 'q27'), (1.031, 'q30'), (1.02, 'q42'), (1.018, 'q20')]
GATE (strict) splayed[answers=PASS warm=FAIL cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=FAIL q16_first=PASS] gen0-ragged[answers=PASS warm=FAIL]
P2 GATE (strict): FAIL
p3_unexpected_materializations none
rss A: 5111900 -> 5968704 kB (x1.168 over all 43, bound 1.1)
rss A without D2 q41,q42: 5084840 -> 5039312 kB (x0.991, bound 1.1, gated)
rss B: 12144580 -> 11965660 kB (x0.985 over all 43, bound 1.1)
P3GATE p2_strict=FAIL materialization=PASS rss=PASS
P3 GATE: FAIL
```

The strict line's failing clauses, by ruling:
- splayed warm: q33 1.068 (202.9 -> 216.6 ms) only (Ruling 10-7: accepted
  with a PR note, post-spec tuning; analysis under Ruling 10-3).  The gap is
  phase 2 of the fused encoded group (`agg_radix_group_fused_fn`, +325 M
  cycles a query, +19% on `agg_radix_group_fn`), reading each partition as
  about 300 runs of about 7 records, one per chunk; the fused phase 1 is
  about 55 M cycles cheaper than the two passes.  Prefetching whole runs,
  prefetching further ahead and gathering the runs first did not help; a
  huge-page record buffer helps generation 0 (q33 205 -> 198 ms) and not
  generation 2.  Larger runs end the fused pass for q33 (212-215 ms) but
  cost q15, q16, q17 and q36 6-16% on generation 2.  Not closed: the fix
  is phase 2's layout, more than a medium change.
- gen-0 splayed warm (C0), every failing clause (Ruling 10-4 extended:
  the sub-ms class; 10b changes no gen-0 code; Task 10's C0 gmean was 0.998):
  - q25 1.111 (0.91 -> 1.01 ms, a 0.10 ms gap over the 0.05 ms floor);
  - q27 1.077 (0.91 -> 0.98 ms, a 0.07 ms gap);
  - gmean (base >= 1 ms, n=34) 1.001 against the 1.00 bound.
- gen-0 parted and ragged warm (Task 10's parted stage; Ruling 10-5,
  accepted).


## P5 float encodings (float store derived from the 10M splayed store, release build, commit f41e6c99)

The tables below this paragraph are **before 8b** (Task 8, f41e6c99).  The
Task 8b numbers (float decode speed) follow them, under "After Task 8b".

Driver: `W=/home/hetoku/data/tmp/p5_bench bench/compress_estimate/p5_bench.sh`
(prep / enc / ident / warm / cold / savet 13:07-13:16; a second prep / ident /
wall pass 15:00-15:05 after the P2/P3 rerun, with full-precision answers).
Binary: f41e6c99 release (sha256 4980fe53bf93; the scripts are the only
change in this commit); BASE for the writer cost: 81c04acb (3073a046ace1).
The store: five F64 columns and one F32 computed from ResolutionWidth,
ClientIP, SendTiming, RegionID, EventTime of `/skull/clickbench/rfsplayed`
(9 999 505 rows), plus IsRefresh as the group key; g0 / g2 saved, measured,
deleted.  9 warm rounds alternated, 3 cold trials (per-file eviction), the
load average checked before each round (4 waits, logged).  Raw data:
/home/hetoku/data/tmp/p5_bench/p5.tsv, summary.txt beside it.

```
| column | encoded B | raw B | ratio | mix |
|---|---|---|---|---|
| price | 8598264 | 79996040 | 9.30x | {const:331 rle:4571 dict:2570 alp:2294} |
| ratio | 17432608 | 79996040 | 4.59x | {raw:159 const:3 rle:9535 dict:64 alp_rd:5} |
| sendt | 1169256 | 79996040 | 68.42x | {const:9185 alp:581} |
| level | 11663056 | 79996040 | 6.86x | {const:3 rle:7290 dict:323 alp:2150} |
| ts | 34648152 | 79996040 | 2.31x | {raw:1 dict:64 alp:9701} |
| f32 | 8284320 | 39998020 | 4.83x | {const:331 rle:8923 dict:17 alp:495} |
| refresh | 1755280 | 19999010 | 11.39x | {const:5 for_bp:8347 rle:1414} |

| column | type | vectors | ALP vecs | ALP exc / rows | ALP (e,f) | ALP_RD vecs | ALP_RD exc / rows | DICT vecs | DICT w/ RAW codes (last vec) | trailers |
|---|---|---|---|---|---|---|---|---|---|---|
| f32 | F32 | 9766 | 495 | 0 / 506880 (0.00%) | (4,1)x495 | 0 | - | 17 | 0 (0) | 0 |
| level | F64 | 9766 | 2150 | 0 / 2201600 (0.00%) | (0,0)x2150 | 0 | - | 323 | 0 (0) | 0 |
| price | F64 | 9766 | 2294 | 0 / 2349056 (0.00%) | (14,12)x2294 | 0 | - | 2570 | 0 (0) | 0 |
| ratio | F64 | 9766 | 0 | - | - | 5 | 92 / 5120 (1.80%) | 64 | 0 (0) | 0 |
| refresh | 3 | 9766 | 0 | - | - | 0 | - | 0 | 0 (0) | 0 |
| sendt | F64 | 9766 | 581 | 59851 / 594944 (10.06%) | (0,0)x581 | 0 | - | 0 | 0 (0) | 0 |
| ts | F64 | 9766 | 9701 | 0 / 9933824 (0.00%) | (0,0)x9701 | 0 | - | 64 | 0 (0) | 0 |

store: g0 459997072 B, g2 83551112 B (x0.182); save g0 1384 ms, g2 50932 ms (prep, one run)

| writer (O5) | save g0 ms | save g2 ms | g2 / g0 | runs |
|---|---|---|---|---|
| p5 | 1320 | 50596 | 38.323 | 3 |
| base | 1611 | 2436 | 1.512 | 3 |

save g2 p5 / base: 20.766; save g0 p5 / base: 0.819
g2 store bytes: p5 83551112, base 365338960; g0: p5 459997072, base 459997072

| query | warm g0 | warm g2 | ratio | first g0 | first g2 | cold g0 | cold g2 | ratio | answers |
|---|---|---|---|---|---|---|---|---|---|
| q0 | 0.00 | 11.93 | 24242.886 | 0.02 | 16.22 | 0.0 | 22.5 | 1055.678 | DIFF (g0 drifts against itself too) |
| q1 | 0.00 | 8.12 | 15735.291 | 0.02 | 11.99 | 0.0 | 12.6 | 752.185 | DIFF |
| q2 | 0.00 | 0.00 | 0.886 | 0.01 | 0.03 | 0.0 | 0.0 | 2.674 | same |
| q3 | 0.00 | 0.00 | 0.939 | 0.01 | 0.03 | 0.0 | 0.0 | 3.063 | same |
| q4 | 7.07 | 12.78 | 1.807 | 20.85 | 24.87 | 177.7 | 31.4 | 0.177 | same |
| q5 | 3.32 | 6.29 | 1.897 | 9.41 | 11.18 | 153.2 | 25.8 | 0.169 | same |
| q6 | 6.56 | 8.54 | 1.302 | 13.10 | 13.42 | 169.5 | 38.4 | 0.226 | same |
| q7 | 5.50 | 1.77 | 0.321 | 11.37 | 6.08 | 158.6 | 6.7 | 0.042 | same |
| q8 | 7.57 | 19.04 | 2.516 | 9.76 | 20.78 | 203.9 | 29.0 | 0.142 | same |
| q9 | 16.58 | 16.65 | 1.004 | 18.69 | 35.96 | 165.4 | 38.4 | 0.232 | same |

| query | wall g0 ms/call | wall g2 ms/call | ratio |
|---|---|---|---|
| q0 | 2.96 | 12.09 | 4.084 |
| q1 | 2.98 | 8.23 | 2.766 |
| q2 | 0.00 | 0.00 | 2.000 |
| q3 | 0.00 | 0.00 | 0.333 |
| q4 | 7.07 | 12.93 | 1.829 |
| q5 | 3.32 | 6.35 | 1.911 |
| q6 | 6.59 | 8.41 | 1.276 |
| q7 | 5.58 | 1.80 | 0.323 |
| q8 | 7.74 | 18.53 | 2.394 |
| q9 | 16.86 | 16.86 | 1.000 |

load waits: 4 (13:10:24/1.79, 15:03:22/2.26, 15:04:07/2.92, 15:04:37/1.77)

**P5 float store, answers g2 = g0: FAIL**
**P5 float store, scan-only warm (no regression, <= 1.05 per query): FAIL** (worst 24242.886)
```

Which store each stage's numbers come from (before 8b): prep / enc /
warm / cold / savet ran on the first derived store (13:07-13:16); ident and
wall ran on a second store, derived and saved again by the same binary at
15:00 after the P2/P3 rerun (same derivation, same bytes: the sizes match).

### After Task 8b: float decode speed

Driver: `W=/home/hetoku/data/tmp/p5b_bench bench/compress_estimate/p5_bench.sh all`
(16:21-16:58), with the fixed summary (`fix(bench)` commit: the scan-only
ratio of a query comes from the wall stage when its timeit g0 median is
below 0.05 ms, wall times below 0.05 ms are floored, every query must be
present in every stage; fix round 1: a failed or unparsable run fails
its query, and where timeit and wall both exist the verdict takes the
larger ratio, a gap over 0.10 printed as CHECK).  Binary: the 8b tree on 7d744e4c, release (sha256
d6b3aefbbbee); BASE for the writer cost 81c04acb.  One store for every
stage: `all` derives and saves it once (prep), then enc / ident / warm /
cold / wall / savet run on it, then clean deletes it.  9 warm rounds, 3 cold
trials, 5 wall rounds, alternated; 7 load waits, logged.  Raw data:
/home/hetoku/data/tmp/p5b_bench/p5.tsv, summary.txt beside it.  The store's
bytes and encodings are unchanged (83 551 112 B g2; the mix and vector
tables above hold).

```
| query | warm g0 | warm g2 | ratio | first g0 | first g2 | cold g0 | cold g2 | ratio | answers |
|---|---|---|---|---|---|---|---|---|---|
| q0 | 0.00 | 2.88 | 5825.818 | 0.02 | 7.30 | 0.0 | 19.2 | 1515.681 | DIFF (g0 drifts against itself too) |
| q1 | 0.00 | 1.94 | 3847.465 | 0.02 | 6.16 | 0.0 | 7.0 | 333.307 | DIFF |
| q2 | 0.00 | 0.00 | 0.940 | 0.01 | 0.03 | 0.0 | 0.0 | 3.398 | same |
| q3 | 0.00 | 0.00 | 1.234 | 0.01 | 0.04 | 0.0 | 0.0 | 2.622 | same |
| q4 | 7.07 | 6.82 | 0.965 | 20.93 | 19.81 | 162.0 | 29.5 | 0.182 | same |
| q5 | 3.26 | 2.25 | 0.690 | 9.44 | 7.02 | 152.9 | 25.8 | 0.169 | same |
| q6 | 6.58 | 6.37 | 0.968 | 13.03 | 11.73 | 153.0 | 37.7 | 0.247 | same |
| q7 | 5.47 | 1.15 | 0.210 | 11.22 | 5.93 | 152.6 | 6.1 | 0.040 | same |
| q8 | 7.56 | 8.74 | 1.157 | 9.76 | 10.41 | 188.5 | 23.7 | 0.126 | same |
| q9 | 16.68 | 16.72 | 1.003 | 18.74 | 27.10 | 149.6 | 33.8 | 0.226 | same |

| query | wall g0 ms/call | wall g2 ms/call | ratio | rounds |
|---|---|---|---|---|
| q0 | 2.956 | 2.873 | 0.972 | 5 |
| q1 | 2.950 | 1.921 | 0.651 | 5 |
| q2 | 0.002 | 0.002 | 1.000 (floored) | 5 |
| q3 | 0.003 | 0.002 | 1.000 (floored) | 5 |
| q4 | 7.046 | 6.798 | 0.965 | 5 |
| q5 | 3.276 | 2.265 | 0.691 | 5 |
| q6 | 6.620 | 6.355 | 0.960 | 5 |
| q7 | 5.525 | 1.118 | 0.202 | 5 |
| q8 | 7.663 | 8.910 | 1.163 | 5 |
| q9 | 16.987 | 16.898 | 0.995 | 5 |

| query | scan-only ratio g2 / g0 | source |
|---|---|---|
| q0 | 0.972 | wall |
| q1 | 0.651 | wall |
| q2 | 1.000 | wall (floored) |
| q3 | 1.000 | wall (floored) |
| q4 | 0.965 | max(timeit 0.965, wall 0.965) |
| q5 | 0.691 | max(timeit 0.690, wall 0.691) |
| q6 | 0.968 | max(timeit 0.968, wall 0.960) |
| q7 | 0.210 | max(timeit 0.210, wall 0.202) |
| q8 | 1.163 | max(timeit 1.157, wall 1.163) |
| q9 | 1.003 | max(timeit 1.003, wall 0.995) |

load waits: 7 (16:22:07/1.63, 16:22:42/1.55, 16:23:32/2.51, 16:24:02/1.59, 16:24:54/1.82, 16:25:24/1.61, 16:26:05/2.13)

**P5 float store, all 10 queries present (and no run failed) in warm, cold, wall and ident: PASS**
**P5 float store, answers g2 = g0: FAIL**
**P5 float store, scan-only warm (no regression, <= 1.05 per query): FAIL** (worst 1.163, q8, max(timeit 1.157, wall 1.163))
```

Before 8b / after 8b, g2 / g0 (the scan-only source of each row; before 8b
q0 / q1 from the wall table, the rest timeit):

| query | before 8b | after 8b |
|---|---|---|
| q0 `sum price` | 4.084 | 0.972 |
| q1 `avg sendt` | 2.766 | 0.651 |
| q4 `price > 12.5` | 1.807 | 0.965 |
| q5 `level == 213.0` | 1.897 | 0.690 |
| q6 `ratio < 0.25` | 1.302 | 0.968 |
| q7 `sendt in [0.5, 1.5)` | 0.321 | 0.210 |
| q8 group `sum price by refresh` | 2.516 | 1.163 (timeit 1.157, wall 1.163) |
| q9 top 10 by price | 1.004 | 1.003 |

What changed (no byte on disk, no answer, no corruption verdict changes):
- **Float fast path** (`dec_fast_float`, enccol.c).  Every float encoding
  decodes straight into the destination at the element width; one
  branch-free pass (or one check per run for CONST / RLE / DICT-over-RLE
  codes) folds the NaN, statistics and null rules.  Anything off declines
  (a trailer, a width, an illegal child, a code outside the dictionary, a
  flag that trips), and the reference `dec_floats` then decodes the vector
  or names its first failing rule with its own error.  DICT over FOR_BP and
  over RLE codes, RLE runs filled in fixed 16-element stores.
- **ALP**: a FOR_BP ints child that `ray_enc_read` would accept whole is
  unpacked and decoded in one pass in enc_alp.c (strict FP flags kept; FMA
  count 0); integers inside [-2^51, 2^51) are converted by the exact
  1.5 * 2^52 trick, which gives (double)n's bits and vectorises, then
  `alp_dec1`'s two multiplies in its order.
- **`ray_enc_unpack`** runs row-major over the lanes (contiguous,
  vectorisable) instead of lane-major (gen-2 only).
- **Float filters**: the single-leaf selection emit (an RLE view's runs
  straight to the selection, P3's integer path) now takes F64 leaves too
  (`ray_enc_eval_cmp_runs_f64`), and `rowsel_emit_runs` writes a passing
  run's indices in 16-index vector stores.

Checks: the decoded bytes of all six float columns, written back as gen 0,
are identical to the g0 store's files (`cmp`).  At one core the 18 sums /
filters / groups over the store give the same answers before and after
(the parallel sums drift run to run, #744).  New test
`encfloat/fast_equals_reference`: every float encoding, F64 and F32, three
lengths, with and without nulls; the fast kernel must take each sound body
with the reference's exact bytes, and over every one-byte forgery of the
body (5 kinds) and statistics narrowed at either end, a body the kernel
takes is one `dec_floats` accepts, with the same bytes.  A mutation (the
per-run statistics check removed) fails it.

Under the fixed summary (fix round 1) q8 reads 1.163, the larger of its
timeit (1.157) and wall (1.163) ratios; it is still the only query over
1.05, and it is accepted as a recorded deviation (Ruling 8b-1).  No row
prints CHECK.

The writer is unchanged by 8b: save g2 p5 / base 20.446, save g0 p5 / base
0.709 (summary.txt of the 8b run).

**The floor (q8).**  q8 is the only query over 1.05.  g0's group is
compute-bound in the dense aggregation (`sum_f64_update`, `count_update`,
`agg_dense_phaseA_fn`: 85% of its cycles).  g2 runs the same aggregation
(and skips g0's `agg_key_bounds_raw`, 11%, by its statistics), and adds the
decode of price and refresh: about 20% of g2's cycles (`dec_fast_float` 10%,
mostly the RLE fill stores; `ray_enc_alp_read` 3.5%; the I16 key 3%; the
range-read dispatch 3%).  Per cores: 1 core 21.6 / 24.4 ms (1.13), 4 cores
10.4 / 11.4 (1.09), 8 threads 7.66 / 8.91 (1.16).  Only one decode per
vector runs (counted).  The decode is about 0.9 cycles per row on one core;
with two threads per core it competes with the sibling's aggregation, which
g0 hides behind its memory latency.  What is left is the materialization
itself: the price vectors are 47% RLE and 26% DICT over RLE codes (median 75
runs per vector), and filling 8 KB per vector is store-bound.  Getting q8 to
1.05 needs the group aggregation to consume runs without materializing them
(a run-aware dense sum), which is outside 8b.

The `(timeit (sum v))` ~0 ms reading reproduces on dev 94a77eeb: `timeit`
releases a lazy result without materializing it inside the timed span.
Filed as #856.

Reading the float-store table (before 8b):
- **Answers FAIL is #744, not P5.**  q2-q9 are equal byte for byte.  q0
  (`sum price`) and q1 (`avg sendt`) differ only in the last digits, and the
  values are nondeterministic run to run.  Eight runs each (full precision,
  /home/hetoku/data/tmp/p5/recheck/drift744.txt):
  - `sum price` gives 7 distinct values on g0 and 8 on g2, all within
    150873431.4403-.4406;
  - `avg sendt` is stable on g0 in this sample (one other run gave
    ...13067), and gives 7 distinct values on g2.

  This is the unordered parallel float fold that dev's #744 fixes.  Gen 2's
  `reduce_src_walk` needs the same chunk-ordered fold at the dev merge
  (already carried).  The first pass printed 6 digits and showed "same" for
  all 10 queries; the driver now writes scalars through `.csv.write`.
- **Scan-only warm FAIL is real and goes to the controller.**  `(timeit q)` on
  a plain generation-0 vector reports a top-level reduction as ~0 ms (q0, q1,
  0.00 ms warm, 0.02 ms first).  The `wall` table is the honest per-call cost
  (process wall time over 100 calls).  On it, g2 is 4.08x on q0, 2.77x on q1,
  1.8-1.9x on q4 / q5, 1.28x on q6 and 2.39x on q8.  q7 is 0.32x (CONST /
  statistics), and q2 / q3 / q9 are at parity or answered from statistics.
  Profile (`perf record`, release binary, 150 calls per query):
  - g2 time is in the decode: `dec_floats` 25-73%, `read_body`, `ray_enc_unpack`;
  - the filters spend 6-16% in `ray_enc_eval_cmp_f64`;
  - g0 is `reduce_range_raw` / `expr_eval_morsel` only.

  `dec_floats` decodes every float vector into a bits buffer, then runs a
  scalar per-row loop over it: the bitmap, the NaN check, the statistics
  bound check and the copy.  The sums cannot use views (float sums decode,
  bit-identical; Task 7).  So every sum, avg, filter and group over a float
  column pays a full decode of about 1 ns per row, on top of the scan.
- **O5, the writer cost.**  Medians of 3 alternated rounds; the table holds
  six float columns and one I16:

  | writer | g0 save | g2 save |
  |---|---|---|
  | f41e6c99 | 1.32 s | 50.6 s |
  | BASE 81c04acb | 1.61 s | 2.44 s |

  So g2 saves 20.8x slower than at BASE, and the g2 store is 4.4x smaller
  (83.6 MB against 365.3 MB).  Per column, each float column takes 6.8-10.0 s
  and the I16 0.92 s.  Profile of one F64 column's save:
  - 61% `alprd_best` → `alprd_plan_rb`: 16 `qsort` calls of 1024 `u16` per
    vector, through glibc's merge sort with a comparison callback;
  - 24% `enc_size` / `ray_enc_stats` (`ray_enc_lt`), the candidate sizing.

  The 16 ALP_RD trials run for every vector, even where ALP or DICT wins.
  Sorting the left parts once at the widest split and reading every
  narrower split from that sorted array (a prefix order) would remove 15
  of the 16 sorts.  A counting pass would remove the sort altogether.
  Post-spec tuning; recorded, not changed here.
- **O1, the ALP sampler.**
  - sendt (SendTiming / 1000) trained (0,0), because most sampled rows of
    chunk 0 are integers (0.0).  Its 581 ALP vectors carry 59 851 exceptions
    over 594 944 rows: 10.06%, about 600 KB of the column's 1.17 MB.
    (3,0) would have none.
  - price (14,12), level (0,0), ts (0,0) and the F32 column (4,1) have 0
    exceptions.
  - ratio is 5 ALP_RD vectors at 1.80% exceptions; the rest of it is RLE.

  The sampler weakness seen in Tasks 2, 5 and 7 therefore shows on real data
  whenever chunk 0 is mostly integral.  Recommendation (post-spec): sample
  from the non-CONST vectors of the chunk, or with an odd stride / offset.
  Or re-train a vector whose exception rate passes a bound against its own
  sample, and add the result to the candidates.  The negative check in
  `encfloat/first_chunk_training` (an alone-trained (e, f) outside the
  chunk-0 candidates) depends on today's sampler.  A comment there now says
  so; re-derive it if the sampler is tuned.
- **The short float DICT tail.**  0 of 3 038 DICT vectors in the store have
  RAW codes, and no column's last (145-row) vector is DICT.  The case
  (Task 6 Minor 4) did not occur on this data.
- #850 does not bear on these numbers.  The derived values come from integer
  columns by division in the engine (no `0.0` literal is parsed into data),
  and ident compares two stores written by the same release binary.

Step 1, the gate runs (logs under /home/hetoku/data/tmp/p5/):

| run | result | runtime errors |
|---|---|---|
| debug `encfloat/`, default cores (2) | 35/35, 0 failed | 0 |
| debug `encfloat/`, `TEST_CORES=0` | 35/35 | 0 |
| debug `encfloat/`, `TEST_CORES=8` | 35/35 | 0 |
| debug full suite | 5964/5964 | 0 |
| debug `equiv/` | 1179/1179 | 0 |
| release FP profile `encfloat/` (incl. `sweep_bit_exact`) | 35/35 | - |
| release FP profile `encfmt/golden` | 4/4 | - |
| release FP profile `idxval/golden` | 3/3 | - |
| informational (O3), release FP profile `aware/` | 94/94 | - |
| informational (O3), release FP profile `idxval/` | 69/69 | - |

- Sweep: 480 saves (2 widths x 6 shapes x 8 lengths x 5 forced encodings),
  NaN payloads and both zeros included, bit-exact (`encfloat/sweep_bit_exact`),
  in the debug build at cores 0 / 2 / 8 and in the release FP profile
  (`make test-release`).  The data is built in C with the zeros as bit
  patterns (D10).
- Codec object flags.  Every line ends with the strict group
  `-fno-fast-math -fno-unsafe-math-optimizations -fno-associative-math
  -fno-reciprocal-math -fsigned-zeros -ffp-contract=off`.  The flags that
  come before it:
  - `enc_alp.o` (debug): `-g -O0 -march=native -DDEBUG -fsanitize=address,undefined ...`;
  - `enc_alp.rel.o`: `-O3 -march=native -funroll-loops -fomit-frame-pointer
    -fno-math-errno -falign-functions=64 -fassociative-math -ffp-contract=fast
    -fno-signed-zeros -fno-trapping-math`;
  - `enc_alp.hard.o`: `-O3 -march=native -g -fno-omit-frame-pointer
    -DRAY_HARDENED -funroll-loops -fno-math-errno -fassociative-math
    -ffp-contract=fast -fno-signed-zeros -fno-trapping-math -falign-functions=64`;
  - `enc_alp.reltest.o`: the release flags plus `-DDEBUG`.

  The full lines are in `cl-*.txt` beside the logs.  FMA instructions in
  `enc_alp.rel.o`: 0 (and 0 in `enc_alp.hard.o`).
- Cross-binary check: the release and debug `./rayforce` write
  byte-identical float column files.  `cmp` gives same for price, ratio,
  sendt, level and f32 on the 1M-row table, with mixes ALP (14,12) / DICT,
  ALP_RD (3 647 exceptions), DICT, RLE / CONST, DICT.  Each binary reads
  each store with identical output: the four md5s are 1b4695e0.  The
  printed sums carry 6 digits.
- Informational, release FP profile (O3): `aware/` 94/94 and `idxval/`
  69/69.  Nothing goes to the final-review watch.
- PR notes:
  - D8: the ALP_RD exception code is 0, a spec clarification;
  - the WASM golden run was not run (ruling; no float wire struct added);
  - the P2 64 KB float scratch is unchanged.

### P2/P3 driver rerun against BASE 81c04acb (I8)

`W=/home/hetoku/data/tmp/p2_bench BASE=81c04acb p2_bench.sh prep` (13:25-13:28),
`splayed` (13:28-13:59), `parted` (13:59-15:00).  P5 binary f41e6c99
(4980fe53bf93), base 81c04acb (3073a046ace1).  The previous data in that
directory (P4's final rerun) moved to `old_p4final_20261015/`.
`p3_summary.py` is unchanged.  Raw data:
/home/hetoku/data/tmp/p2_bench/p2.tsv and summary.txt.

```
totals over all 43: warm 1705.9 vs 1644.7 (0.964); first run 2522.7 vs 2713.4 (1.076); 3-run 5996.4 vs 6092.4 (1.016)
answer_mismatches none
scan_only(gated) n=28; gmeans over base>=1ms (per column): warm n=22 0.588, first n=23 0.707, cold n=23 0.338; worst warm=1.241
scan_only_list_but_materializing none
worst5 warm [(1.241, 'q5'), (1.179, 'q6'), (1.109, 'q7'), (1.068, 'q4'), (1.068, 'q33')]
totals over all 43: warm 4625.0 vs 2875.6 (0.622); first run 16086.7 vs 13743.0 (0.854); 3-run 25520.0 vs 19602.9 (0.768)
answer_mismatches none
scan_only(gated) n=34; gmeans over base>=1ms (per column): warm n=33 0.179, first n=33 0.251, cold n=33 0.144; worst warm=1.027
scan_only_list_but_materializing none
worst5 warm [(1.027, 'q33'), (1.013, 'q15'), (1.007, 'q32'), (1.007, 'q29'), (1.007, 'q13')]
totals over all 43: warm 1706.1 vs 1705.8 (1.000); first run 2530.0 vs 2532.6 (1.001); 3-run 6001.3 vs 6031.9 (1.005)
answer_mismatches none
gen0 warm_gmean(base>=1ms n=35)=0.998 warm_worst=1.073 worst5 [(1.073, 'q27'), (1.032, 'q7'), (1.024, 'q30'), (1.02, 'q1'), (1.019, 'q5')]
totals over all 43: warm 4635.3 vs 4636.9 (1.000); first run 16037.0 vs 16116.2 (1.005); 3-run 25492.0 vs 25536.2 (1.002)
answer_mismatches none
gen0 warm_gmean(base>=1ms n=42)=0.999 warm_worst=1.036 worst5 [(1.036, 'q1'), (1.008, 'q33'), (1.008, 'q30'), (1.007, 'q19'), (1.007, 'q18')]
q16 first run base 97.9 ms, p2 99.1 ms (carry 2)
totals over all 43: warm 4387.9 vs 4390.5 (1.001); first run 15795.6 vs 15839.8 (1.003); 3-run 24713.9 vs 24767.2 (1.002)
answer_mismatches none
gen0 warm_gmean(base>=1ms n=42)=0.999 warm_worst=1.014 worst5 [(1.014, 'q31'), (1.01, 'q4'), (1.009, 'q5'), (1.009, 'q30'), (1.009, 'q12')]
enc A: columns with DELTA vectors: ['UserID {raw:76 const:3 delta_bp:97 rle:9526 dict:64}]
enc B: columns with DELTA vectors: ['UserID {raw:75 const:3 delta_bp:89 rle:8963 dict:640}]
sizes {('A', 'g0'): 5948677939, ('A', 'g2'): 3216455899, ('B', 'p0'): 4807844987, ('B', 'p2'): 2072366907}  peak_rss_kb {('A', 'g0'): 5112180, ('A', 'g2'): 5966708, ('B', 'p0'): 12160960, ('B', 'p2'): 11981660}  evict_modes fadvise  ragged_partition ['9', '40']  meta {'p2': ['f41e6c99', '4980fe53bf93'], 'base': ['81c04acb', '3073a046ace1']}
GATE (strict) splayed[answers=PASS warm=FAIL cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=FAIL] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (strict): FAIL
GATE (P3-exempt) splayed[answers=PASS warm=FAIL cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS first_total=PASS] parted[answers=PASS warm=PASS cold=PASS first=PASS materializing_warm=PASS no_unexpected_materialization=PASS] gen0-splayed[answers=PASS warm=FAIL] gen0-parted[answers=PASS warm=PASS q16_first=FAIL] gen0-ragged[answers=PASS warm=PASS]
P2 GATE (P3-exempt): FAIL
p3_unexpected_materializations none
rss A: 5112180 -> 5966708 kB (x1.167 over all 43, bound 1.1)
rss A without D2 q41,q42: 5070228 -> 5042840 kB (x0.995, bound 1.1, gated)
rss B: 12160960 -> 11981660 kB (x0.985 over all 43, bound 1.1)
P3GATE p2_strict=FAIL materialization=PASS rss=PASS
P3 GATE: FAIL
```

**P2/P3 rerun strict line: FAIL by the letter (`P2 GATE (strict): FAIL`,
`P3 GATE: FAIL` through `p2_strict`).  Every failing clause is one P4 ruled,
at the same size.**  Materialization PASS, RSS PASS, and answers PASS on
every arm.  Sizes are byte-identical to P4's rerun (A g0 5 948 677 939,
g2 3 216 455 899; B p0 4 807 844 987, p2 2 072 366 907): the ClickBench
store has no float column.

1. **Splayed warm (A), q33 1.068.**  P4's rerun gave 1.068 too (Ruling
   10-7, post-spec tuning).  q4-q7 above 1.05 are 0.00 ms rows under the
   sub-ms floor.
2. **Gen-0 splayed warm (C0), q27 1.073**, from 0.92 to 0.98 ms, a gap of
   0.06 ms over the 0.05 ms floor (Ruling 14-8's class; P4 had q25 / q27).
   Recheck: 12 alternated processes, 10 warm runs each, on a fresh g0 copy:
   base 1.020 ms, P5 1.004 ms, ratio 0.985
   (/home/hetoku/data/tmp/p5/recheck/q27.txt).  The gmean is 0.998.
3. **Gen-0 parted q16_first**: P5 99.1 ms against base 97.9 ms (1.013).  The
   clause requires P5 to be strictly below base, and P4 accepted the same
   clause (Ruling 10-5).  Gen-0 parted warm passes this time (gmean 0.999,
   worst 1.036 for q1), and so does ragged (0.999, worst 1.014).

**P5 gate (float round trip bit-exact, NaN payloads included): PASS**
