# Local mutation (#950)

The workload appends 500 rows to an ordinary three-column table and amends
10 elements of a 500-element vector per step. Each run starts from empty
state. The logical rows and vector contents match across implementations;
physical storage and each language's append idiom can differ.

Build and run the native global/local comparison:

```sh
make release -j4
./rayforce -c 1 -f bench/local_mutation/local_mutation.rfl
```

For a fresh-process local-state comparison with installed q, PeachQ and Amber:

```sh
python3 bench/local_mutation/compare.py --rayforce ./rayforce \
  --output /tmp/local-mutation-results.json
```

The runner detects `q`, `peachq`, and the bare `amberx` launcher on PATH;
explicit paths can be supplied with the corresponding options. Amber needs
its q libraries in `AMBER_HOME` or `--amber-home` (default `/usr/share/amber`).
The interactive `amber` wrapper is not used. Each engine warms up at 10
steps, then runs five alternating samples at each size in each of three
fresh processes. Engine order rotates between processes. All 120 samples
check the returned row count and final vector sum. Timings exclude startup,
library loading and printing. Rayforce uses `timeit`; the q-family script
uses `.z.p`. Other work on the host can affect these observations.

Local measurements on 2026-10-23, median milliseconds of 15 samples per size:

| Runtime | 1,000 steps / 500k rows | 2,000 steps / 1M rows |
|---|---:|---:|
| Rayforce with #950 | 3.973 | 7.763 |
| q | 1.880 | 4.024 |
| PeachQ | 10.952 | 22.077 |
| Amber | 0.720 | 1.311 |

`results.csv` records all samples; `environment.json` records
the measured executables and Amber library hashes. These are observations,
not performance assertions in the test suite or claims of identical storage.

`functional.rfl` reproduces the old local workaround, rebinding the result
of functional insert and incrementing the vector functionally. Run it with an
unmodified release binary:

```sh
rayforce -c 1 -f bench/local_mutation/functional.rfl
```

One baseline run on the same host took 5,445 ms and 22,821 ms at the two
sizes, with row-count and vector-sum checks inside the workload. The main
improvement is removal of quadratic table copying. This change reuses the
existing table-append and indexed-vector-amend fast paths; it does not make
every list/concat/storage representation allocation-free. Physical pointer
checks in `lang/local_mutation_fast_paths` verify unique local table/vector
engagement and that a real alias still forces a copy.
