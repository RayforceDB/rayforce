# Staged grouped SYM strlen

A narrow warm-query benchmark, not a complete engine performance gate. Requires
both binaries to support generation-2 columns (for example, #902's integration
base and the staged-strlen optimization). Do not use a released gen0-only binary
as the gen2 baseline.

The deterministic fixture has 10,000,000 rows, 997 I64 keys, and five nullable SYM
values, plus the other columns from the original six-column differential fixture.
It measures `avg(strlen(y)) by k asc k` on gen0/gen2 splayed tables. Empty symbols
are excluded from AVG's divisor. Setup writes under `bench/staged_strlen/data/`;
allow approximately 2 GiB of RAM and 1 GiB of disk. Data is retained for repeated
runs and ignored by git. Do not use this directory for application data.

Build and preserve separate release binaries before running. From the repository
root, the optional Python 3 standard-library runner performs setup, shuffles
baseline/candidate and gen0/gen2 process order with seed 902512, and reports the
median of seven warm samples for each of three fresh processes:

```sh
python3 bench/staged_strlen/compare.py /absolute/base /absolute/candidate --setup --cores 8
```

No Python is required to execute the fixture manually:

```sh
/absolute/base -c 8 bench/staged_strlen/setup.rfl
/absolute/base -c 8 bench/staged_strlen/gen0.rfl
/absolute/base -c 8 bench/staged_strlen/gen2.rfl
/absolute/candidate -c 8 bench/staged_strlen/gen0.rfl
/absolute/candidate -c 8 bench/staged_strlen/gen2.rfl
```

Each timed process loads its table and evaluates the query once before timing.
The checksum is only a rendered sanity check; use the regression/differential
suite for answer and type equivalence. Run without concurrent builds/tests,
record binary/compiler/CPU details, and do not interpret these as cold-read,
writer, high-cardinality, or general speedup measurements. The main CLI takes
`-c N`; `RAYFORCE_CORES` is the test harness setting, not its core-count option.
