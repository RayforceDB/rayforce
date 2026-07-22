---
id: F-0002
title: Documentation and public API tests release errors with a no-op helper
severity: major
dimension: api-doc-consistency
unit:
  - docs/docs/c-api/core.md
  - test/test_public_api.c
  - include/rayforce.h
status: superseded-by-class
class: CF-0001
attempts: 0
pass: P-01
updated: 2026-07-22
---

## Evidence

`docs/docs/c-api/core.md`, section “RAY_IS_ERR(p)” (lines 488–493), teaches:

```c
if (RAY_IS_ERR(result)) {
    printf("Error: %s\n", ray_err_code(result));
    ray_release(result);
    return 1;
}
```

`test/test_public_api.c` repeats the same cleanup in
`test_public_get_error_trace_populated` (line 592) and
`test_public_get_error_trace_cleared_on_eval` (line 604):

```c
ray_release(err);
```

The ownership norm in `include/rayforce.h`, immediately above
`ray_error_free` (lines 237–241), says the opposite:

> Free a RAY_ERROR object.  ray_release() is a deliberate no-op for error ray_t* (see src/mem/cow.c), so callers that hold the sole reference and want the block reclaimed must use this helper instead — otherwise the error leaks until heap teardown.

and declares:

```c
void ray_error_free(ray_t* err);
```

## Why this is a defect

The primary error-handling example and its public API tests normalize a cleanup
operation the public header explicitly says leaks. A long-lived embedder that
copies the example retains every error object until heap teardown.

## How to verify the fix

Search these units for cleanup of values already proven by `RAY_IS_ERR`:
`rg -n 'ray_release\((result|err)\)' docs/docs/c-api/core.md
test/test_public_api.c`. Every such owned error must be reclaimed according to
the `ray_error_free` contract, and the focused public API tests must still pass.

## Validation

Validated. All three quoted cleanup calls remain. `src/mem/cow.c`,
`ray_release`, still returns immediately for `RAY_IS_ERR(v)`, confirming that
the documented/test cleanup does not reclaim error objects and that
`ray_error_free` is required for owned errors.

## Census

The public-contract cleanup pattern recurs four times across the C API
reference and public-header conformance test. The full reproducible census is
recorded in CF-0001. Because four meets the configured class threshold of
three, this finding is superseded by CF-0001 pending human triage.

## Objection


## Remediation

Superseded by CF-0001 during RP-0001; no point fix was applied.


## Verification
