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

## Scope and coordination

Before starting, check existing issues and pull requests for related work. If
someone is already working on the same outcome, coordinate in that thread and
build on the existing effort rather than opening a competing fix. State the
intended scope so contributors and reviewers know what is being taken on.

Keep one coherent user-visible outcome in one PR, including its implementation
and regression tests. Track related missing cases in a checklist rather than
opening a separate issue and PR for each case; use focused commits to organize
the work internally. Keep the PR in draft while the agreed scope is incomplete,
and address review feedback in the same PR. When consolidating existing PRs,
link them and account for their changes and unresolved reviews before closing
the superseded ones. Leave already-merged work alone, and do not bundle unrelated
changes merely to reduce the PR count.

## Opening a pull request

1. Branch off the latest `dev`.
2. Make your change. Keep commits focused; we use
   [Conventional Commits](https://www.conventionalcommits.org/)
   (`feat:`, `fix:`, `perf:`, `docs:`, `refactor:`, `test:`, `chore:`) — the
   release notes are generated from these prefixes, so they matter.
3. Make sure the build is clean and tests pass locally:

   ```sh
   make            # builds (debug: ASan + UBSan)
   make test       # fast suite (no sanitizers, all cores); `make test-full`
                   # runs every test under ASan + UBSan, as release PRs do
   ```

   Tests run in parallel worker processes, each running many tests in turn.
   A test that fails only after some other test ran in the same process
   prints a reproduce line; run it to replay that exact sequence in one
   process, then trim the file it names down to the culprit:

   ```sh
   ./rayforce.test.fast --replay /tmp/rayforce_replay.<pid>.<n>
   ```

4. Open the PR against **`dev`**. CI runs on every pull request (there are no
   push-triggered runs — the PR run tests the exact merge result).

## Local build notes

- Zero external dependencies; C17 toolchain only.
- `make` compiles with `clang` when `CC` is not set, so a machine without clang
  fails at the first compile. `make CC=gcc` (or `CC=gcc` in the environment)
  builds with gcc. The fuzz, TSan, coverage and `compdb` targets use clang
  regardless of `CC`.
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

Each PR is built with these compilers (`.github/workflows/ci.yml`): gcc for
the Ubuntu debug and release legs, clang for the macOS debug and release
legs, upstream Clang 23 from apt.llvm.org for the Ubuntu `clang-current` test
leg, and the Ubuntu runner's `clang` for the NDEBUG, fuzz-smoke and TSan
legs.

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
