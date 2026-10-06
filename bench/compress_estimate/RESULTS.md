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
