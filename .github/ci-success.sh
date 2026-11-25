#!/usr/bin/env bash
# The ci-success verdict (ci.yml).  Reads the needed jobs' results from
# R_<job> and the tier from TIER (cheap | queue | release); exit 0 = pass.
#
# One tolerance, merge queue only: the macOS debug leg of build-and-test may
# be one GitHub never gave a runner (hosted macOS capacity: the job is
# cancelled after three 5-minute acquisition attempts with no step run, and
# its check run carries the annotation "The job was not acquired by Runner
# ...").  Then the matrix reports a non-success result although no test ran
# and failed; that leg runs again on the push to dev (macos-dev.yml).  The
# tolerance needs, in EVERY attempt of the run so far:
#   - the jobs listing read whole (every page; jobs read == total_count);
#   - its build-and-test legs exactly the queue matrix (QUEUE_LEGS below);
#   - the macOS debug leg never acquired: cancelled, no step, no runner,
#     at least 14 minutes from creation to completion, and the annotation
#     above on its check run (unreadable annotations fail closed);
#   - every other leg success.
# Anything else (a macOS job that started, one cancelled while queued, a
# real failure in an earlier attempt, a listing that cannot be read) fails
# the check as before.  Pull requests into master are strict.
#
#   ci-success.sh                      the verdict, from the environment
#   ci-success.sh --classify FILE      "tolerated" or "fail: <reason>" for a
#                                      bundle the verdict collects:
#     {"expected": [leg names],
#      "attempts": [{"attempt": n, "total_count": n, "jobs": [...]}, ...],
#      "annotations": {"<job id>": [annotation, ...], ...}}
set -uo pipefail

# The queue tier's build-and-test legs, as ci.yml's matrix names them.  If
# the matrix changes and this does not, the tolerance never applies.
MAC_LEG='build-and-test (macos-latest, debug, clang)'
QUEUE_LEGS='["build-and-test (ubuntu-latest, debug, gcc)",
             "build-and-test (ubuntu-latest, release, gcc)",
             "build-and-test (macos-latest, debug, clang)"]'
NOT_ACQUIRED='The job was not acquired by Runner'
MIN_QUEUED=840   # seconds; three acquisition attempts take about 15 minutes

classify() {
  jq -r --arg mac "$MAC_LEG" --arg na "$NOT_ACQUIRED" --argjson min "$MIN_QUEUED" '
    .annotations as $ann | .expected as $exp
    | def never_acquired:
        .conclusion == "cancelled"
        and ((.steps // []) | length) == 0
        and ((.runner_name // "") == "")
        and ((.created_at // "") != "") and ((.completed_at // "") != "")
        and ((.completed_at | fromdateiso8601) - (.created_at | fromdateiso8601)) >= $min
        and (($ann[.id | tostring] // null) | type == "array"
             and any(.[]; (.message // "") | contains($na)));
    [ .attempts[] | . as $a
      | [.jobs[] | select(.name | startswith("build-and-test ("))] as $legs
      | if (.jobs | length) != .total_count then
          "attempt \($a.attempt): read \(.jobs | length) of \(.total_count) jobs"
        elif ($legs | map(.name) | sort) != ($exp | sort) then
          "attempt \($a.attempt): legs \($legs | map(.name)) are not the matrix"
        else
          ($legs[] | select(.name == $mac and (never_acquired | not))
                   | "attempt \($a.attempt): \(.name) is not a never-acquired job (\(.conclusion))"),
          ($legs[] | select(.name != $mac and .conclusion != "success")
                   | "attempt \($a.attempt): \(.name) \(.conclusion)")
        end ] as $why
    | if (.attempts | length) == 0 then "fail: no attempt read"
      elif ($why | length) == 0 then "tolerated"
      else "fail: " + $why[0] end' "$1" 2>/dev/null || echo "fail: unreadable bundle"
}

# the bundle for this run, from the jobs and check-runs APIs; fails if any
# page cannot be read
collect() {
  local d=$1 a api="repos/${GITHUB_REPOSITORY}"
  for a in $(seq 1 "$GITHUB_RUN_ATTEMPT"); do
    gh api --paginate --slurp "${api}/actions/runs/${GITHUB_RUN_ID}/attempts/${a}/jobs?per_page=100" > "$d/p$a" || return 1
    jq --argjson a "$a" '{attempt: $a, total_count: .[0].total_count, jobs: [.[].jobs[]]}' "$d/p$a" > "$d/j$a" || return 1
  done
  jq -s '.' "$d"/j* > "$d/attempts" || return 1
  echo '{}' > "$d/ann"
  for id in $(jq -r --arg mac "$MAC_LEG" '.[].jobs[] | select(.name == $mac) | .id' "$d/attempts"); do
    gh api --paginate --slurp "${api}/check-runs/${id}/annotations" > "$d/n$id" || return 1
    jq --arg id "$id" --slurpfile n "$d/n$id" '. + {($id): ($n[0] | add // [])}' "$d/ann" > "$d/ann2" \
      && mv "$d/ann2" "$d/ann" || return 1
  done
  jq -n --argjson exp "$QUEUE_LEGS" --slurpfile at "$d/attempts" --slurpfile an "$d/ann" \
    '{expected: $exp, attempts: $at[0], annotations: $an[0]}'
}

if [ "${1:-}" = "--classify" ]; then classify "${2:?bundle JSON file}"; exit 0; fi

ok=1
want() {   # job, the result its tier requires
  r=$(printenv "R_$1"); echo "$1: $r (want $2, tier $TIER)"
  [ "$r" = "$2" ] || ok=0
}
bt=$(printenv R_build_and_test)
if [ "$TIER" = queue ] && [ "$bt" != success ]; then
  d=$(mktemp -d)
  if collect "$d" > "$d/bundle"; then v=$(classify "$d/bundle"); else v="fail: could not read the run's jobs or annotations"; fi
  rm -rf "$d"
  if [ "$v" = tolerated ]; then
    echo "ci-success: tolerated: the macOS debug leg was never given a runner (cancelled after the acquisition attempts, no step, in every attempt); it runs on the push to dev"
    R_build_and_test=success; export R_build_and_test
  else
    echo "ci-success: no tolerance: $v"
  fi
fi
want build_and_test success
want docs success
q=skipped; [ "$TIER" = cheap ] || q=success
for j in clang_current gcc_12 fuzz_smoke tsan; do want "$j" "$q"; done
n=skipped; [ "$TIER" = release ] && n=success
want ndebug "$n"
[ "$ok" = 1 ] || { echo "ci-success: a job this tier requires did not pass"; exit 1; }
