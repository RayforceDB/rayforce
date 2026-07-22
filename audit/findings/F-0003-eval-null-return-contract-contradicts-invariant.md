---
id: F-0003
title: ray_eval_str null-return contract contradicts the public value invariant
severity: major
dimension: api-doc-consistency
unit: include/rayforce.h
status: superseded-by-class
class: CF-0002
attempts: 0
pass: P-01
updated: 2026-07-22
---

## Evidence

`include/rayforce.h`, immediately above `RAY_ASSERT_VALUE` (lines 208–210),
states the public value norm:

> A value position (an eval'd / builtin-returned ray_t*) is never a bare C NULL — value-null is always RAY_NULL_OBJ.

The contract immediately above `ray_eval_str` in the same header (lines
666–670) contradicts it:

> Parse and evaluate a Rayfall source string against the global env. Returns NULL for void / null results, an error ray_t* on failure (test with RAY_IS_ERR and inspect with ray_err_code), or the result value otherwise.

## Why this is a defect

Embedders cannot determine from the public header whether an evaluated null is
tested with `p == NULL` or `RAY_IS_NULL(p)`. Those alternatives imply different
safe control flow because the surrounding API explicitly distinguishes bare C
NULL from `RAY_NULL_OBJ`.

## How to verify the fix

Re-read both quoted contracts and compile/run a focused public API test that
calls `ray_eval_str` on a void/null expression. The observed return and both
header comments must agree on exactly one representation; the test must assert
that representation explicitly.

## Validation

Validated. Both contradictory header comments remain. The current evaluator
returns `RAY_NULL_OBJ` for no-value paths, and the existing
`test_eval_println` assertion explicitly confirms `RAY_IS_NULL(result)`, so the
`ray_eval_str` comment is the stale side of the contract.

## Census

Exact searches for comments coupling `ray_eval_str` with bare NULL null/void
results found three instances: `include/rayforce.h:667`, `test/main.c:247-248`,
and `test/stress_eval.c:69`. The complete procedure is in CF-0002. Three meets
the class threshold, so this finding is superseded pending triage of CF-0002.

## Objection


## Remediation

Superseded by CF-0002 during RP-0001; no point fix was applied.


## Verification
