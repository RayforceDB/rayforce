# Compressed-columns P0 measurements and gate decision

## Machine and commit

- commit a8a3e5e6 (branch feat/compressed-columns, worktree compress-p0), release build
- 8 cores (`nproc`), 62 GB RAM (`free -g`), kernel 6.8.0-100-generic
- Store: local 10M splayed ClickBench table, /skull/clickbench/rfsplayed, 9 999 505 rows, 105 columns, symfile at `<db>/.sym`
- The 100M store was unavailable (user ruling); no remote machine was used. The submission's parted store (/skull/tmp/rayforce_tests/current) does not exist locally and was not run.
- Estimator runtime on the 10M store: 88 s, single run.

Note on indexes: the store has no separate index files; HASH indexes are attached to the column files. Five columns carry one (WatchID, ClientEventTime, FUniqID, RefererHash, URLHash). UserID has none in this store (the estimator was run with `--keep-index UserID`, as the brief says, so that scenario treats UserID's index as kept).

## Whole-table breakdown (10M)

```
breakdown rows=9999505 data=3599827376 index=1141598560 sym=1207249691 other=2312 total=5948677939
scenario today                      bytes=5948677939 size_ratio=1.000 score_factor=1.0000
scenario columns encoded            bytes=3216321381 size_ratio=1.850 score_factor=0.9404
scenario columns + sym fsst         bytes=2673585034 size_ratio=2.225 score_factor=0.9231
scenario columns + sym fsst + kept ix bytes=1531989226 size_ratio=3.883 score_factor=0.8731
```

The estimator's "kept ix" scenario keeps only the UserID index (none exists). The measured index use below says RefererHash and URLHash must be kept, which adds 194 915 360 + 195 932 480 bytes: 1 922 837 066 bytes, ratio 3.094, score factor 0.8932 (3.094^-0.1; the factors above follow ratio^-0.1).

## Per-type column ratios (sum of `col` lines; encoded includes null bitmaps and DICT)

| type | columns | raw bytes | encoded bytes | ratio | index bytes |
|---|---|---|---|---|---|
| I16 | 48 | 959 955 456 | 140 402 890 | 6.84 | 132 096 |
| I32 | 19 | 759 963 520 | 128 669 616 | 5.91 | 52 288 |
| I64 | 6 | 479 976 576 | 260 169 632 | 1.84 | 1 034 508 992 |
| SYM (ids) | 28 | 1 119 945 456 | 152 978 952 | 7.32 | 0 |
| TIMESTAMP | 3 | 239 988 288 | 184 686 688 | 1.30 | 106 902 432 |
| DATE | 1 | 39 998 080 | 563 040 | 71.04 | 2 752 |
| **all** | 105 | 3 599 827 376 | 867 470 818 | **4.150** | 1 141 598 560 |

Columns that barely compress (ratio below 1.5): WatchID 0.99, HID 1.03, RefererHash 1.03, URLHash 1.18, ClientEventTime 1.20, TIMESTAMP EventTime/LocalEventTime 1.35, RefererCategoryID 1.35, Referer 1.41, URL 1.47 (SYM id columns). Full `col` lines are in /tmp/ce_10m.txt (106 lines, not committed).

## Encoding mix (fraction of chunk bytes)

```
RAW 0.0370  CONST 0.3960  FOR_BP 0.1639  DELTA_BP 0.0001  RLE 0.3836  DICT 0.0194
```

## Predicate groups (skip rates)

```
group g_adv  chunk_decided=0.0721 vector_decided=0.1840 undecided=0.7439 pred_bytes=793464 undecided_bytes=773448
group g_mpm  chunk_decided=0.0000 vector_decided=0.0725 undecided=0.9275 pred_bytes=1284168 undecided_bytes=1278504
group g_sp   chunk_decided=0.0000 vector_decided=0.0140 undecided=0.9860 pred_bytes=11463472 undecided_bytes=11462376
group g_url  chunk_decided=0.8296 vector_decided=0.1468 undecided=0.0236 pred_bytes=26816336 undecided_bytes=612656
group g_ref  chunk_decided=0.0000 vector_decided=0.0002 undecided=0.9998 pred_bytes=27909920 undecided_bytes=27905040
group g_user chunk_decided=0.7013 vector_decided=0.2745 undecided=0.0243 pred_bytes=16247048 undecided_bytes=1085416
group g_q37  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=1765176 undecided_bytes=177216
group g_q40  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=82436744 undecided_bytes=6069864
group g_q41  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=69231040 undecided_bytes=5910280
group g_q42  chunk_decided=0.9279 vector_decided=0.0107 undecided=0.0614 pred_bytes=1765176 undecided_bytes=177216
```

All rows=9 999 505. The q37-q42 groups (CounterID == 62 plus a date range) are decided at chunk level for 92.8% of rows because the table is ordered by CounterID/EventDate. g_ref (Referer != 0) and g_sp/g_mpm/g_adv are mostly undecided: those predicates select a large share of rows.

## Symfile

```
sym file=1207249691 fsst=664513344 ratio=1.82 snapshot=1274980311 decode_s=3.544 count=8466329
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
| .sym | 1.207 GB | 12.1 GB | 11.3 GB | within 7% (symfile grows sub-linearly in distinct strings, not exactly linear; the 10M file is 1.07x of 1/10 of the 100M one) |

Caveats: encoded size depends on chunk-level locality, so per-chunk behaviour is assumed unchanged at 10x rows (same data ordering, same chunk size; 100M column files are only more chunks). Index size is the hash table, close to linear in rows. The symfile and snapshot depend on distinct-string count, which is not guaranteed linear. Skip rates are ratios and do not extrapolate by row count; they are only valid if the 100M table has the same ordering. Decode time is extrapolated at 33-35 s only if per-block cost is flat.

## Snapshot vs cold read

- Cold read: could not be measured. `sudo -n tee /proc/sys/vm/drop_caches` failed ("a password is required"); not retried. Warm read of the symfile (page cache): 0.119 s for 1.2 GB.
- Decode: 3.544 s single thread for 1.2 GB at 10M, about 360 MB/s; the warm read is 30x faster than the decode. The decode is therefore slower than any warm read and is bounded by CPU, not IO.
- Extrapolated to 100M: snapshot about 12.0 GB held in heap, decode about 33-35 s single thread (same 360 MB/s), versus a cold read of an 11.3 GB symfile whose time depends on the disk (not measured here).

## Decisions

- **P0 gate: PASS.** Column data is 4.15x smaller (3.60 GB -> 0.867 GB at 10M, in line with the spec's 4.1x at 100M), well above the 1.5x threshold; the whole-table breakdown and projection (1.85x columns only, 2.225x with sym FSST, 3.09x with index trimming) and per-query index use are reported. Caveat: 100M was not measured, only extrapolated.
- **PI input:** keep RefererHash (194.9 MB, used by q41) and URLHash (195.9 MB, used by q42). No benchmark query consulted WatchID (508.4 MB), FUniqID (135.2 MB) or ClientEventTime (106.9 MB): candidates to stop persisting, 750.5 MB of 1 141.6 MB at 10M (66%), about 7.5 GB at 100M. Only 2 of 43 queries consult any index, once each, on a splayed load; on the parted load the submission uses, none.
- **PS gate input:** snapshot is 1.275 GB at 10M (about 12 GB at 100M) with a 3.5 s (about 33 s) single-thread decode. On a 32 GB machine, the 12 GB snapshot plus the 8.6 GB encoded columns is about 20.6 GB, leaving about 11 GB for the working set and page cache; this fits but is tight, while a snapshot beside 36 GB of raw columns does not. A cold read time was not measured. Recommendation: block cache, not a full snapshot, because the snapshot costs 12 GB of heap (37% of a 32 GB machine) and a decode of tens of seconds at open, and queries touch only a small part of the 8.5M symbols. Revisit if a parallel decode brings the open cost under the cold read time.
