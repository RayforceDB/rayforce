# Identity left-join gather

The flat LEFT join executor retains left columns when the completed left
index mapping is exactly `[0, ..., left_rows - 1]`. It avoids allocating
and gathering unchanged columns and preserves their null bitmaps, string
pools and symbol domains. Updates retain the existing copy-on-write
semantics. Duplicate expansion and reordered output use the gather path;
FULL joins also keep that path because right-only rows change the left side.

## Standalone probe

Build separate release binaries from the base and candidate, then run each
from this checkout:

```sh
RAYFORCE_CORES=4 /path/to/base bench/join_identity/probe.rfl
RAYFORCE_CORES=4 /path/to/candidate bench/join_identity/probe.rfl
```

The probe builds one million rows with eight left columns and three unique
right keys. It measures one join, excluding input construction, and checks
the left row order, the last payload column, right payload sum and unmatched
null count. Each process prints `BENCH|milliseconds|valid|rows`. Compare fresh
processes in randomized order after discarded warm-ups, with identical build
flags and worker settings. Whole-process timing includes construction and
validation; use the printed join timing when comparing the executor itself.
The probe checks values; C ownership tests verify that sharing actually occurs.

## Backtester measurement

Matched local release builds based on `d7153e72`, Clang 23.1.3, default
`make release` flags; the candidate changes only `join.c`. Both variants
use the same projected backtester report. The workload has 100 instruments,
26,304 hourly steps, 2,630,400 bars and 705 fills, using xsmomf. The default
worker configuration is unchanged between variants.

One discarded warm-up per variant, then three fresh processes each in
shuffled order (seed 22), without concurrent local compilation or tests:

| Median | Base | Candidate |
|---|---:|---:|
| Post-run checks | 1139.00 ms | 1033.27 ms |
| Whole process | 4.007 s | 3.917 s |
| Peak RSS | 1607.8 MiB | 1588.6 MiB |

Check ranges: base 1120.70–1146.35 ms, candidate 1013.85–1056.53 ms.
Process ranges: base 3.997–4.017 s, candidate 3.898–3.921 s.
This small experiment on a shared host is evidence for this workload,
not a general speedup guarantee. The backtester and its generated data are
external to this repository; the standalone probe above needs neither.

## Regression coverage

`test/test_join_buildside.c` checks pointer sharing, copy-on-write, null
metadata, string-pool lifetime after releasing the source, duplicate
expansion and a large order-preserving join. The large fixture keeps its
build side below radix dispatch: radix partitioning may reorder rows, which
must fail the explicit identity guard even when cardinality is unchanged.

`test/rfl/join/join_identity.rfl` checks nulls, symbols, nested lists,
source/result update isolation, missing matches, duplicate expansion and
FULL joins.
