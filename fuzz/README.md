# Fuzzing

Coverage-guided fuzz targets for the highest-value untrusted-input surfaces.
Linux + clang only (the system toolchain on macOS does not bundle the fuzzing
runtime).

## Targets

| Target | Entry point | Surface |
|---|---|---|
| `fuzz_parse`    | `ray_parse`            | Rayfall source parser / lexer |
| `fuzz_numparse` | `ray_parse_i64/f64/u64_hex` | numeric-literal scanners (pure, no runtime) |
| `fuzz_de`       | `ray_de` / `ray_de_raw` | binary wire-frame / journal-entry decoder |
| `fuzz_eval`     | `ray_eval_str`         | full parse → eval pipeline (sandboxed) |
| `fuzz_csv`      | `ray_read_csv*`        | CSV reader (input via memfd) |
| `fuzz_journal`  | `ray_journal_validate` / `ray_journal_replay` | journal framing walk + replay |
| `fuzz_parquet`  | `ray_parquet_open` / `ray_parquet_next` | footer, page, encoding and optional index parsers |
| `fuzz_select_merge` | nested `select` vs materialized `select` | two/three-layer queries, nullable numeric/SYM/STR columns, serialized results and errors |

The journal log is a sequence of IPC frames verbatim, so the `de` corpus also
hardens journal replay's decode step.

`fuzz_eval` runs untrusted programs in-process, so the escape hatches that would
compromise the fuzzer — shell (`.sys.exec`/`.sys.cmd`), `exit`/`quit`, network
`listen`/`.ipc.open`, and file writes (`write-csv`, `.log.open`) — are compiled
out under `-DRAY_FUZZING` (guards in `syscmd.c` / `system.c` / `builtins.c` /
`journal.c`). `fuzz_csv` and `fuzz_journal` expose their input as an anonymous
in-memory file (`memfd` → `/proc/self/fd`), so there is no disk I/O.

Two accepted `fuzz_eval` limitations: global state (`set`) leaks across
iterations, so a rare crash may not reproduce standalone; and loop/recursion
constructs can run long, so a `timeout-*` artifact is only a finding when the
input has no such construct.

## Running

```sh
make fuzz-parse                 # 60s (default) run of one target
make fuzz-parse FUZZ_RUNTIME=0  # run until a crash / Ctrl-C
make fuzz-smoke                 # short pass over the fast targets (PR CI gate)
make fuzz-select_merge FUZZ_RUNTIME=600 # structured nested-query differential check
```

`select_merge` generates two/three-layer queries and compares them with the
same queries wrapped in `do` to force inner materialization. It checks serialized
results and error codes, allowing relative/absolute tolerance `1e-12` only for
F64 aggregate values (fusion changes addition order). Row expressions, schema,
order and null metadata still compare exactly.

`fuzz-smoke` covers `parse`, `numparse`, and `de` (the fast, stateless
targets); `eval`, `csv`, `journal`, and `select_merge` run in the nightly `fuzz-long` job
(`.github/workflows/nightly.yml`), 15 minutes each over a cached corpus.

Grown corpora live in `fuzz/corpus/<target>/` (gitignored); committed starter
inputs live in `fuzz/seeds/<target>/`.  Regenerate the working corpora from the
test suite before a session:

```sh
scripts/fuzz-seed-parse.sh      # parse/eval inputs from test/rfl
scripts/fuzz-seed-frames.sh     # binary frames captured from a journaling server
```

### Local note: AddressSanitizer + high-entropy ASLR

On some newer kernels the sanitizer runtime intermittently faults at process
exit under the default ASLR entropy — unrelated to any target code (it also
happens on an empty input).  If you see nondeterministic `SEGV`s at teardown,
run under reduced ASLR:

```sh
setarch -R make fuzz-parse
```

CI runners are unaffected.  If clang auto-selects a gcc toolchain directory
without `libstdc++` (a partially-installed newer gcc shadowing the real one),
pass the real one:

```sh
make fuzz-smoke FUZZ_LDEXTRA=-L/usr/lib/gcc/x86_64-linux-gnu/<ver>
```

## Crash triage workflow

```
crash-XXXX appears (CI artifact or local run)
  1. confirm:   ./build_fuzz/fuzz_<t> crash-XXXX
  2. minimize:  ./build_fuzz/fuzz_<t> -minimize_crash=1 -runs=20000 crash-XXXX
  3. fix on the dev branch (normal PR flow)
  4. regression test:
       language-expressible bug  -> test/rfl/regress/<name>.rfl   (expr !- code)
       binary-input bug          -> keep the minimized input in fuzz/seeds/<t>/
  5. retain the minimized input under fuzz/seeds/<t>/ so the corpus never
     forgets the case
```

Error assertions in `.rfl` regression tests match the 7-byte error **code**
(e.g. `!- parse`, `!- domain`), not the detailed message — the message is not
part of the value rendered for comparison.

`make fuzz-parquet FUZZ_RUNTIME=600` exercises the native Parquet footer,
page, dictionary, definition-level and optional index parsers under ASan/UBSan.
Its seeds are ordinary small Parquet files, including both page versions,
Snappy, checksums, malformed optional indexes and fidelity edge cases. The
driver consumes at most 256 batches of 257 rows per input to bound work from
forged row counts. It covers decoded/pruned reader paths; it does not certify
import publication, parallel scheduling or every supported producer.
