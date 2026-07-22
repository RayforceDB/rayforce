---
id: CF-0002
title: ray_eval_str comments still claim value-null is bare NULL
severity: major
dimension: api-doc-consistency
unit:
  - include/rayforce.h
  - test/main.c
  - test/stress_eval.c
status: fixed
class: null
members:
  - F-0003
attempts: 0
pass: P-01
created-by: RP-0001
updated: 2026-07-22
---

## Pattern

A comment describing `ray_eval_str` says a void/null value is represented by
bare C `NULL`, contradicting the public value invariant and current evaluator,
which use `RAY_NULL_OBJ`.

Reproduce the census with:

```sh
rg -n -i 'ray_eval_str.*(returns? NULL|NULL = void/null|void results)' \
  include src docs test README.md examples fuzz bench
```

## Census

- `include/rayforce.h:667`, the public declaration says “Returns NULL for
  void / null results”.
- `test/main.c:247-248`, the Rayfall harness says `ray_eval_str` returns NULL
  for void results and describes bare-NULL comparison semantics.
- `test/stress_eval.c:69`, the stress harness annotates the result with
  “NULL = void/null result, fine”.

Three instances meet the project threshold of three.

## Validation

Re-ran the recorded census on 2026-07-22. It reproduced the two single-line
harness comments but missed the public-header instance because `ray_eval_str`
and “Returns NULL” are on separate lines. A supplementary exact-literal
census reproducibly finds all three recorded instances:

```sh
rg -n -i 'Returns NULL for void / null results|ray_eval_str returns NULL for void results|NULL = void/null result' \
  include src docs test README.md examples fuzz bench
```

The public invariant in `include/rayforce.h` states that value-null is always
`RAY_NULL_OBJ`; `test_eval_println` and `test_eval_null_keyword` assert that
behavior directly. All three stale comments therefore remain substantively
confirmed.


## Root cause

The evaluator's null-result contract moved to the `RAY_NULL_OBJ` value
invariant, but comments in the public header and two harnesses were not migrated
with it.

## Global strategy

Rung (b): update all three comments to the `RAY_NULL_OBJ` contract and make
the public invariant the named source of truth. Retain defensive bare-NULL
checks where they protect against allocation failure, but do not describe bare
NULL as a successful evaluation result. Re-run the focused null-evaluation
test and the recorded census.

## Remediation

Planned in `audit/plans/RP-0003.md` using the human-accepted rung-(b)
strategy. The direction is the existing public `RAY_NULL_OBJ` invariant; no
new or different norm choice is introduced.

Updated the public `ray_eval_str` contract in `include/rayforce.h` and the
harness comments in `test/main.c` and `test/stress_eval.c` to identify
`RAY_NULL_OBJ` as the successful void/null result. Defensive bare-NULL paths
remain unchanged and are described only as no-result fallbacks.

Self-checks completed on 2026-07-22:

- The supplementary exact-literal census returned zero stale instances.
- The original regex returned one documented non-defect exception: the
  corrected `test/main.c` comment, because its broad `void results` alternative
  also matches `RAY_NULL_OBJ` wording.
- Focused tests `lang/eval/println` and `lang/eval/null_keyword` each passed
  1/1 with no skips or failures.


## Verification

Pending independent Verifier review.
