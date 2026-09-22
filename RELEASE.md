# Release process

Rayforce uses a tag-driven release flow. **The git tag is the single source of
truth for the version** — no version literal is ever hand-edited in source.

## Branch model

- **`dev`** — integration upstream and the **repository default branch**.
  Feature/fix branches merge here; all contributions target `dev` (see
  [CONTRIBUTING.md](CONTRIBUTING.md)). CI runs on every push and PR.
- **`master`** — protected, stable releases only. The only way in is a merged
  release PR. A PR opened against `master` that isn't the `dev -> master` release
  PR is automatically moved to `dev` by the **Retarget stray PRs** workflow.

## Cutting a release

1. Make sure `dev` is green and ready.
2. Open a pull request **`dev` → `master`** with the title set to the new
   version: **`vX.Y.Z`** (e.g. `v2.1.3`).
   - The **Version Guard** check validates the title: it must be a clean
     `vX.Y.Z`, must not already be tagged, and must be strictly greater than the
     latest release.
3. Once CI and Version Guard are green, **merge** the PR (or enable
   auto-merge so it merges itself when checks pass).
4. On merge, the **Release** workflow automatically:
   - creates the tag `vX.Y.Z` at the merge commit,
   - builds the optimized binary for Linux and macOS with the version injected
     at compile time (`make dist RAY_VERSION=X.Y.Z`),
   - packages `rayforce-X.Y.Z-<os>-<arch>.tar.gz` + a `.sha256` checksum,
   - publishes a GitHub Release with a feature-oriented changelog and the
     artifacts.

That's the whole ritual. **Never edit the version in source to make a release** —
the tag is authoritative.

### Manual / re-run path

The Release workflow also has a `workflow_dispatch` trigger. Run it from the
Actions tab (or `gh workflow run release.yml -f version=X.Y.Z`) to re-drive a
release that half-finished, or to seed the very first release before the
automatic `pull_request: closed` path can fire. It is idempotent: if a draft
`vX.Y.Z` already exists it is reused and built from its own target commit; a
manual dispatch with no prior draft tags the current `master` HEAD.

> **Why the build checks out a commit, not the tag:** a *draft* GitHub release
> does **not** create its git tag — the tag ref only appears when the release is
> published (`--draft=false`). So `build` checks out the release's target
> **commit**; the `publish` job is what creates the tag `vX.Y.Z`. Checking out
> `ref: vX.Y.Z` during build would fail (the tag does not exist yet).

## Release notes

Notes are a **feature changelog, not a contributor ledger**. The `prepare` job
runs [`.github/release-notes.sh`](.github/release-notes.sh), which groups
conventional-commit subjects since the previous release tag into sections —
**✨ Features** (`feat`), **🐛 Bug fixes** (`fix`), **⚡ Performance** (`perf`),
**📚 Documentation** (`docs`) — with everything else collapsed under
*Maintenance & internal*. Author names and merge-commit noise are dropped; a
**Full changelog** compare link at the bottom is the exhaustive per-commit view.

Because notes are scoped to `previousTag..thisRelease`, they only read well once
there is a previous release tag to diff against — the first release (`v2.1.0`)
spans the entire v2 line and was given a hand-written highlights summary instead.

## How the version reaches the binary

The Makefile resolves the version (CI override `RAY_VERSION=` > latest git tag >
`0.0.0` dev default) and injects it at compile time via `-D`, exactly like it
already does for the git commit and build date. `ray_version_string()`, the REPL
banner, and `.sys.build` all report that value. The literals in
`include/rayforce.h` are only `#ifndef`-guarded `0.0.0` fallbacks for untagged
builds and for downstream code that includes the header without our `-D` flags.

A plain local `make` on an untagged commit reports `0.0.0`; with tags present it
reports the latest via `git describe`. To build a specific version locally:

```sh
make dist RAY_VERSION=2.1.3
```

## One-time repository setup (GitHub)

- **Default branch = `dev`**: so `git clone` and the "New pull request" button
  steer contributors at the integration branch, not `master`. This is the
  primary nudge; Version Guard is the backstop, and **Retarget stray PRs**
  (`.github/workflows/retarget-prs.yml`) catches the few that still land on
  `master` and moves them to `dev`. Because it is a `pull_request*` workflow, it
  only governs `master` once the file is present on `master` — i.e. after the
  first release PR that carries it merges.
- **Branch protection / ruleset on `master`**: require a pull request before
  merging; require these two status checks to pass, and disallow direct pushes:
  - **`ci-success`** — a single aggregator job in `ci.yml` that goes green only
    when every CI matrix leg (ubuntu/macOS × debug/release) passed. Require this
    one instead of the four matrix names so renaming the matrix can't silently
    drop the gate.
  - **`check-version`** — the Version Guard job that validates the release
    version in the PR title.
- No release secret or bypass token is needed — the default `GITHUB_TOKEN`
  (`contents: write`) creates the tag and the release. The release workflow never
  pushes a commit to `master`, only a tag ref.
- **Optional — Zulip announcement**: add a repository secret **`ZULIP_API_KEY`**
  (the API key of the `releases-bot@rayforcedb.zulipchat.com` bot). The `announce`
  job then posts each published release — title + changelog highlights + link —
  to **#Announcements** under the **Rayforce** topic. Without the secret the job
  is skipped, and a Zulip error never fails the release (`continue-on-error`).
  If Zulip rejects the post, the bot may need to be a *Generic bot* (not just an
  incoming-webhook) and subscribed to the Announcements channel.
- **Optional — Homebrew tap**: create an empty repo **`RayforceDB/homebrew-tap`**
  and add a repository secret **`HOMEBREW_TAP_TOKEN`** (a PAT — fine-grained or
  classic — with **contents: write** on `homebrew-tap`; the default
  `GITHUB_TOKEN` can't push to a *different* repo). The `homebrew` job then
  rewrites `Formula/rayforce.rb` in the tap on every release. Without the secret
  the job is skipped. Users install with `brew install rayforcedb/tap/rayforce`.

## Packaging / install channels

Each release publishes, in addition to the source:

- **Tarballs** — `rayforce-X.Y.Z-linux-x86_64.tar.gz` and
  `…-darwin-arm64.tar.gz` (+ `.sha256`). The Linux one is **portable**
  (`RAY_MARCH=x86-64-v3`, AVX2/~2013+) so it can't SIGILL on a different/older
  CPU. macOS stays `-march=native`: the runner is the oldest Apple-Silicon
  class, so it's already a safe floor (arm64 has no x86-style optional-ISA
  traps, and baselining to `armv8-a` would only cost M1 tuning).
- **`.deb`** (`rayforce_X.Y.Z_amd64.deb`) — the same portable Linux binary,
  packaged with nfpm ([`packaging/nfpm.yaml`](packaging/nfpm.yaml)). No setup
  (uses `GITHUB_TOKEN`).
- **Homebrew** (`rayforcedb/tap`) — a **build-from-source** formula
  ([`packaging/homebrew-formula.rb.tmpl`](packaging/homebrew-formula.rb.tmpl)),
  auto-bumped by the `homebrew` job. Compiling on the user's machine means all
  Mac arches work and there's no redistribution-portability footgun.

> Want maximum per-CPU performance instead of a portable binary? Build from
> source — `make` and Homebrew both default to `-march=native`. Only the
> *distributed* x86-64 artifacts use the `x86-64-v3` baseline.

## Platform support

Linux and macOS binaries are published today. Windows builds and passes the
test suite from source with the MSYS2 CLANG64 toolchain (see CONTRIBUTING.md),
but no Windows binary is published yet; to ship one, add a `windows-latest`
row (MSYS2 CLANG64) to the `build` matrix in `.github/workflows/release.yml`.
