#!/usr/bin/env bash
# Make a merged release commit an ancestor of `dev`.
#
# Merging a release PR leaves a squash or merge commit on `master` that `dev`
# lacks. Until `dev` contains it, the next release PR is out of date with its
# base and, after a squash, shows conflicts that aren't real. A `-s ours` merge
# fixes the ancestry without touching a file, which is correct only when `dev`
# already has everything the release shipped, so that is checked first: the
# release must have the same tree as the PR head, and `dev` must contain the PR
# head. Anything else means `master` carries changes of its own, which need a
# person to merge them, so the script stops with an error.
#
# Usage: release-merge-back.sh <X.Y.Z> <pr-number> <release-sha> <pr-head-sha>
# Runs from a checkout with full history whose `origin` it can push `dev` to.
# Safe to re-run: it does nothing once `dev` contains the release.
set -euo pipefail

version="$1"
pr="$2"
release="$3"
head="$4"

fail() {
  echo "::error title=Merge-back to dev failed::$1 Follow the merge-back step in RELEASE.md by hand, or the next release PR will be out of date with master."
  exit 1
}

git fetch --quiet origin dev master

git cat-file -e "$release^{commit}" 2>/dev/null || fail "Release commit $release not found."
git cat-file -e "$head^{commit}" 2>/dev/null || fail "PR head $head not found."

if ! git diff --quiet "$head" "$release"; then
  fail "Release $release differs from PR head $head, so master has changes of its own."
fi

for attempt in 1 2 3 4 5; do
  git fetch --quiet origin dev

  if git merge-base --is-ancestor "$release" origin/dev; then
    echo "dev already contains $release."
    exit 0
  fi
  if ! git merge-base --is-ancestor "$head" origin/dev; then
    fail "dev does not contain PR head $head."
  fi

  git checkout --quiet --detach origin/dev
  git merge --quiet --no-ff -s ours "$release" \
    -m "chore: merge master back into dev after the v$version release (#$pr)"
  git diff --quiet origin/dev HEAD || fail "The merge changed files on dev."

  # dev may have moved since the fetch; start over from its new tip.
  if git push --quiet origin HEAD:refs/heads/dev; then
    echo "Merged $release back into dev."
    exit 0
  fi
  echo "Push to dev failed (attempt $attempt of 5)."
  [ "$attempt" = 5 ] || sleep "$((attempt * 5))"
done

fail "Could not push to dev. Check that the PAT secret is valid and may bypass dev's required checks."
