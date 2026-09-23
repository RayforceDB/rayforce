# Windows vs Linux sanity check

Not a performance study: a coarse check that the Windows port lands in the
same ballpark as Linux, run while porting (`serhii/windows-port`).  The two
sides do not share a compiler, an allocator-visible kernel, or a filesystem,
so only order-of-magnitude gaps are meaningful here.

## Environment

**CPU**: 11th Gen Intel Core i7 (8 logical cores) — one laptop, both runs
**Windows**: Windows 11, clang 20.1.8 (MSYS2 CLANG64), 32 GiB
**Linux**: WSL2 (kernel 6.6.87.2-microsoft-standard-WSL2, Ubuntu 24.04), gcc 13.3.0, 15 GiB to the VM
**Build**: `make release` both sides (`-O3 -march=native`, no sanitizers — `nm bench-alloc | grep -ci asan` → 0)

WSL2 is a virtual machine with its own memory budget and a virtualised
filesystem; that alone moves I/O and page-fault numbers. Treat the file-backed
rows as indicative only.

## Allocator micro-benchmark (`bench/alloc`)

| case | Windows | Linux |
|------|---------|-------|
| atom-64B | 85.7 Mops/s | 80.9 Mops/s |
| vec-256B | 85.5 Mops/s | 78.6 Mops/s |
| morsel-8K | 88.2 Mops/s | 80.3 Mops/s |
| morsel-16K | 88.4 Mops/s | 78.2 Mops/s |
| large-1M | 62.7 Mops/s | 56.4 Mops/s |
| producer-consumer | 9.2 Mops/s, peak RSS 28 MB | 6.0 Mops/s, peak RSS 68 MB |

## Engine operations (5M rows, `timeit`, median of 3)

| operation | Windows (ms) | Linux (ms) | Win/Lin |
|-----------|-------------:|-----------:|--------:|
| arith-f64 (`sum (* f 1.5)`) | 7.94 | 5.10 | 1.56 |
| sort-i64 | 9.89 | 8.78 | 1.13 |
| distinct-i64 | 2.96 | 4.69 | 0.63 |
| group-by sym | 3.44 | 3.93 | 0.88 |
| select where | 7.13 | 14.14 | 0.50 |
| inner-join | 108.6 | 108.3 | 1.00 |
| csv write (5M rows) | 2201 | 1654 | 1.33 |
| csv read | 155 | 186 | 0.83 |
| splayed set | 509 | 580 | 0.88 |
| splayed get + count | 5.9 | 0.28 | 21 |

`sum`/`avg` are omitted: both platforms report ~0.005 ms, the DAG elides them.

## Reading

Compute paths agree within a factor of ~1.6 either way, which is compiler and
noise territory, and the allocator is slightly ahead on Windows.

The one real gap is `splayed get + count` (5.9 ms vs 0.28 ms). It opens and
maps one file per column, so it measures file-open cost, not the engine:
`CreateFileA` + `CreateFileMapping` + `MapViewOfFile` per column, plus
whatever on-access scanning is installed. The absolute cost is small and it is
paid per table open, but a wide table opened in a loop would feel it.
