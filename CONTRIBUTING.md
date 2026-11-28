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
changes merely to reduce the PR count. Each fix is its own pull request: the
merge queue batches landings, so there are no roll-up pull requests.

## Opening a pull request

1. Branch off the latest `dev`.
2. Make your change. Keep commits focused; we use
   [Conventional Commits](https://www.conventionalcommits.org/)
   (`feat:`, `fix:`, `perf:`, `docs:`, `refactor:`, `test:`, `chore:`) — the
   release notes are generated from these prefixes, so they matter. A change
   that removes or renames something users call, or changes an answer they
   rely on, ends its **commit message** with a `BREAKING CHANGE:` trailer
   naming what changed and what to use instead (`BREAKING CHANGE: stddev_pop
   is removed; use dev_pop.`). It goes in the message's last paragraph, after a
   blank line, on one line or continued on indented lines, with the other
   trailers (`Fixes #n`). The release notes list it under **Breaking changes**
   ([RELEASE.md](RELEASE.md#release-notes)).
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

4. Open the PR against **`dev`**. It lands through the merge queue; see
   [How a pull request lands on dev](#how-a-pull-request-lands-on-dev).

## How a pull request lands on dev

`dev` is protected by a merge queue. The required checks are
**`rayforce-audit`** and **`ci-success`**.

1. **On the pull request**, CI runs the cheap tier: the Linux gcc debug build
   with `make test`, the Linux gcc release build with the static and shared
   libraries, and the docs (`mkdocs build --strict`, `make docs-check`). The
   `rayforce-audit` check reviews the diff. Each push re-runs both.
2. The branch does **not** need to be up to date with `dev`: do not merge
   `dev` into it or rebase it only to catch up. The queue tests it against the
   `dev` it will land on.
3. **Before queueing**, make the commit message final. With one commit, the
   queue squash-merges with that commit's subject and message (the repository's
   settings are `COMMIT_OR_PR_TITLE` and `COMMIT_MESSAGES`), not the pull
   request's title or description. With more than one, it takes the pull
   request's title and joins the commits' messages. So the subject must be a
   conventional commit (`fix(scope): ...`) and match the pull request's title.
   Everything that must reach `dev` goes in the commit message: the
   explanation, and the trailers in its last paragraph (`Fixes #n`,
   `BREAKING CHANGE: ...`). The description mirrors the commit message.
4. **Queue it** once `rayforce-audit` and `ci-success` are green:
   `gh pr merge <n> --squash`, or the **Merge when ready** button.
5. The queue builds it on top of the pull requests ahead of it and runs the
   queue tier: the cheap tier plus macOS debug, `clang-current`, gcc 12,
   ThreadSanitizer, a short fuzz smoke and the advisory cppcheck.
   `rayforce-audit` reports success there without auditing again.
   GitHub's hosted macOS runners are sometimes all busy: when it gives the
   macOS debug leg no runner at all (it cancels the leg after about 15
   minutes without running a step and annotates it "The job was not
   acquired by Runner"), `ci-success` lets the pull request land without
   it, and says so in its log. It checks every attempt of the run, so a
   re-run does not forgive an earlier failure. A leg that ran and failed,
   or one cancelled while waiting, still fails the queue. macOS debug also
   runs on every push to `dev`, as its own check ("macOS on dev"), so such
   a change gets its macOS test there; a run in progress finishes, and a
   newer push replaces only a run still pending.
6. On green it is squash-merged into `dev`. On a failure it is removed from
   the queue: fix it, push, and queue it again.

A pull request into `master` (the release PR) runs the full release tier, with
no tolerance for a leg that did not run: the
queue tier plus macOS release, the NDEBUG suite, the full-length fuzz smoke,
and `make test-full` under ASan + UBSan.

## Documentation

A change that adds, renames or removes a builtin, or changes what it accepts or
returns, updates the documentation **in the same PR**. A function that is
registered but undocumented cannot be found by the people who need it, and the
reference drifts further from the code with every PR that skips this.

For a new builtin that means:

1. Give it an entry in `src/lang/sig.c`: its usage line (the call shape error
   messages, `.repl.signature` and `:h` show) and its one-line summary (what it
   does). The file is the one source of both: the summary is what
   `.repl.signature` and `:h` answer, and the reference pages are generated
   from it. A `/* docs: ... */` comment after the entry adds more text to the
   builtin's row in `docs/docs/reference/all-functions.md` only.
2. Run `make docs-gen`. It writes the **Registered Builtin Surface** section of
   `docs/docs/reference/all-functions.md` (every registered name, by arity,
   with its summary) and the Description cell of every row for the builtin in
   the "Function" tables of that page and of `docs/docs/namespaces/*.md`.
   Commit what it writes; do not edit those cells by hand.
3. Give it a row in the matching category table of the reference page, with
   its arity, flags (`restricted`, `special`, ...) and a working example, and
   run `make docs-gen` again to fill its description.
4. If it belongs to a namespace (`.idx.*`, `.parquet.*`, ...), document it on
   that namespace's page under `docs/docs/namespaces/` as well.
5. If it is blocked under `-U`, add it to the restricted-mode summary in
   `docs/docs/namespaces/index.md`.

Run the check before opening the PR:

```sh
make docs-check     # python3 scripts/check-builtin-docs.py
```

It fails when a registered builtin has no summary in `src/lang/sig.c`, when a
page differs from what `make docs-gen` writes, when a registered builtin is
missing from the surface list, or when the list names something that is no
longer registered or lists it under a different arity than its registration
call (`register_unary`, `register_binary`, `register_vary`). Whether a summary
is right is for the author and the reviewer; `system/usage_coverage` checks
that every builtin has one from the running engine as well.

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
than running it. On Windows `.sys.exec` runs its command through `cmd.exe`,
so any shell syntax needs the marker: quoting, pipes, redirection, `$(…)`,
`mkdir -p` (a `cmd.exe` builtin there), `printf`, `test`, or a child
`./rayforce`. A plain `rm -f` / `rm -rf` of paths as cleanup does not: the
Windows setup below puts MSYS2's `rm` on `PATH`, and the runner moves every
`/tmp` path under its own root. Keep the platform-neutral assertions in an
unmarked file and move the shell checks to a marked one, and write a text
fixture with `(write path text)` rather than `printf`. C tests use `#ifndef RAY_OS_WINDOWS` / `SKIP(...)` for the
same purpose. Prefer the shell-free helpers `ray_test_rm_rf` /
`ray_test_mkdir_p` (`test/test.h`) over `system("rm -rf …")` in C tests.

The test driver puts its scratch files under the directory `TMPDIR` names, so set
it to an existing disk-backed directory to keep them off a RAM-backed `/tmp`.

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
gate the merge queue (fuzz smoke + TSan on every merge group) and run deeper
overnight.

The compilers (`.github/workflows/ci.yml`): gcc for the Ubuntu debug and
release legs, gcc 12 for a release and shared build, clang for the macOS
legs, the current upstream Clang for the Ubuntu `clang-current` test leg, and
the Ubuntu runner's `clang` for the NDEBUG, fuzz-smoke and TSan legs.

- **Fuzzing** (Linux + clang): coverage-guided targets under `fuzz/` cover the
  parser, numeric scanners, the binary wire/journal decoder, and the sandboxed
  evaluator, CSV, and journal-replay paths. `make fuzz-smoke` runs the smoke targets locally;
  `make fuzz-<target> FUZZ_RUNTIME=<secs>` runs one locally; the merge queue
  fuzzes each target for 20 s, a release PR for 60 s. See
  [`fuzz/README.md`](fuzz/README.md), including the crash-triage workflow.
- **ThreadSanitizer**: `make tsan-test TSAN_FILTER=<pool|heap|parallel|stress>`
  runs the concurrency suites under TSan at 4 cores. Every entry in
  `.tsan-suppressions` must justify why the report is benign.
- **Static analysis** (advisory): `make tidy` (clang-tidy, correctness-only
  checks) and `make cppcheck`.
- **Nightly** (`.github/workflows/nightly.yml`): deep randomized stress, the full
  suite under TSan, long fuzz runs, and a server soak (`scripts/soak.sh`) that
  kill/restarts the server and watches resident memory for leaks.
