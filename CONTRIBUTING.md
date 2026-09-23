# Contributing to Rayforce

Thanks for helping improve Rayforce. This page covers the one thing that trips
people up most often — **which branch to target** — plus the basics of getting a
change merged.

## Branch model (read this first)

Rayforce uses a two-branch flow:

- **`dev`** — the integration branch and the repository default. **All
  contributions target `dev`.** Open your PR here.
- **`master`** — release-only. The single PR it ever receives is the
  `dev -> master` release PR (titled `vX.Y.Z`), which a maintainer cuts. Direct
  feature/fix PRs against `master` are blocked by the **Version Guard** check.

`dev` is the default branch, so a fresh `git clone` and the "New pull request"
button already point the right way. If you do open a PR against `master` by
mistake, a bot automatically retargets it to `dev` and leaves a note — nothing
for you to fix.

See [RELEASE.md](RELEASE.md) for how releases are cut.

## Opening a pull request

1. Branch off the latest `dev`.
2. Make your change. Keep commits focused; we use
   [Conventional Commits](https://www.conventionalcommits.org/)
   (`feat:`, `fix:`, `perf:`, `docs:`, `refactor:`, `test:`, `chore:`) — the
   release notes are generated from these prefixes, so they matter.
3. Make sure the build is clean and tests pass locally:

   ```sh
   make            # builds (debug: ASan + UBSan)
   make test       # runs the test suite
   ```

4. Open the PR against **`dev`**. CI runs on every pull request (there are no
   push-triggered runs — the PR run tests the exact merge result).

## Local build notes

- Zero external dependencies; C17 toolchain only.
- Debug builds enable AddressSanitizer + UndefinedBehaviorSanitizer; warnings are
  errors (`-Werror -Wall -Wextra`). A warning will fail CI, so fix them locally.
- The version reported by a local build is derived from git tags (`0.0.0` on an
  untagged checkout) — never hand-edit a version literal in source.

### Build flavours

Each flavour compiles to its own object files, so they never clobber one another.

| Target | What it produces |
|---|---|
| `make` / `make debug` | default: `-O0 -g` with ASan + UBSan (the flavour tests run under) |
| `make release` | `-O3 -march=native` with FP-reassociation flags |
| `make hardened` | the cloud tier: release optimisation, but keeps `-g`/frame pointers and links `-rdynamic` so the crash handler (`src/core/crash.c`) prints a symbolized backtrace; defines `RAY_HARDENED` (keeps cheap invariant checks live) |
| `make tsan` / `make tsan-test` | ThreadSanitizer build (clang; mutually exclusive with ASan) — see below |
| `make coverage` | clang source-based coverage → `coverage_html/` |
| `make compdb` | `compile_commands.json` for editors/analyzers |

## Tests

Behavioural tests live under `test/rfl/<group>/<name>.rfl` and are
auto-discovered. Add or update a `.rfl` test alongside any behaviour change. Run
a focused subset with:

```sh
./rayforce.test -f <substring>
```

A `.rfl` file that needs a POSIX shell or filesystem (fixtures built or
checked through `.sys.exec`, `/proc`, `/dev/tcp`, …) carries the line
`;; @requires: posix`; on Windows the runner reports it as `SKIP` rather
than running it. C tests use `#ifndef RAY_OS_WINDOWS` / `SKIP(...)` for the
same purpose. Prefer the shell-free helpers `ray_test_rm_rf` /
`ray_test_mkdir_p` (`test/test.h`) over `system("rm -rf …")` in C tests.

### Windows

Build with the MSYS2 CLANG64 (or MINGW64) toolchain — `pacman -S
mingw-w64-clang-x86_64-clang make` — from an MSYS2 shell or with
`C:\msys64\clang64\bin` and `C:\msys64\usr\bin` on `PATH`:

```sh
make            # debug build (ASan + UBSan)
make test
make release
```

The debug binaries load the ASan runtime DLL from `clang64\bin`, so keep it on
`PATH` when running them.

## Stability tooling

Beyond the ASan/UBSan test run, the repo carries a stability toolset. These
gate CI (fuzz smoke + TSan on every PR) and run deeper overnight.

- **Fuzzing** (Linux + clang): coverage-guided targets under `fuzz/` cover the
  parser, numeric scanners, the binary wire/journal decoder, and the sandboxed
  evaluator, CSV, and journal-replay paths. `make fuzz-smoke` is the PR gate;
  `make fuzz-<target> FUZZ_RUNTIME=<secs>` runs one locally. See
  [`fuzz/README.md`](fuzz/README.md), including the crash-triage workflow.
- **ThreadSanitizer**: `make tsan-test TSAN_FILTER=<pool|heap|parallel|stress>`
  runs the concurrency suites under TSan at 4 cores. Every entry in
  `.tsan-suppressions` must justify why the report is benign.
- **Static analysis** (advisory): `make tidy` (clang-tidy, correctness-only
  checks) and `make cppcheck`.
- **Nightly** (`.github/workflows/nightly.yml`): deep randomized stress, the full
  suite under TSan, long fuzz runs, and a server soak (`scripts/soak.sh`) that
  kill/restarts the server and watches resident memory for leaks.
