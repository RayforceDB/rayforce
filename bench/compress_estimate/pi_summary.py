#!/usr/bin/env python3
"""Summarize pi.tsv: medians, ratios and the PI branch decision."""
import statistics, sys
from collections import defaultdict

rows = defaultdict(list)
conv = {}
for line in open(sys.argv[1]):
    kind, variant, item, value = line.rstrip("\n").split("\t")
    if kind == "conv":
        conv[(variant, item)] = value
    elif kind == "meta":
        print(f"meta\t{item}\t{value}")
    elif kind in ("warm", "warm3", "cold", "cold_load"):
        rows[(kind, variant, item)].append(float(value))
    elif kind == "evict":
        rows[("evict", variant, item)].append(value)

lazy = "no"
for q in ("q41", "q42"):
    if ("warm3", "gen2", q) in rows:
        h = statistics.median(rows[("warm3", "hash", q)])
        for v in ("nohash", "gen2"):
            m = statistics.median(rows[("warm3", v, q)])
            print(f"warm3\t{q}\t{v}_ms={m:.3f}\thash_ms={h:.3f}\tratio={m / h:.3f}\truns={len(rows[('warm3', v, q)])}")
for q in ("q41", "q42"):
    for kind in ("warm", "cold", "cold_load"):
        h = statistics.median(rows[(kind, "hash", q)])
        n = statistics.median(rows[(kind, "nohash", q)])
        print(f"{kind}\t{q}\thash_ms={h:.3f}\tnohash_ms={n:.3f}\tratio={n / h:.3f}\t"
              f"runs={len(rows[(kind, 'hash', q)])}/{len(rows[(kind, 'nohash', q)])}")
        if kind == "warm" and n / h > 1.05:
            lazy = "yes"
modes = sorted({m for k, v in rows.items() if k[0] == "evict" for m in v})
print("evict_modes\t" + ",".join(modes))
for v in ("hash", "nohash", "gen2"):
    if (v, "wall_s") not in conv:
        continue
    print(f"conv\t{v}\twall_s={conv[(v, 'wall_s')]}\tpeak_rss_kb={conv[(v, 'peak_rss_kb')]}\t"
          f"store_bytes={conv[(v, 'store_bytes')]}")
for c in ("WatchID", "ClientEventTime", "FUniqID", "RefererHash", "URLHash", "UserID"):
    print(f"col\t{c}\thash={conv[('hash', 'bytes_' + c)]}\tnohash={conv[('nohash', 'bytes_' + c)]}\tgen2={conv.get(('gen2', 'bytes_' + c), '-')}")
print(f"LAZY_BUILD_NEEDED\t{lazy}")
