---
id: CF-0001
title: Public error-cleanup examples use ray_release on owned errors
severity: major
dimension: api-doc-consistency
unit:
  - docs/docs/c-api/core.md
  - docs/docs/c-api/dag.md
  - test/test_public_api.c
status: fixed
class: null
members:
  - F-0002
attempts: 0
pass: P-01
created-by: RP-0001
updated: 2026-07-22
---

## Pattern

An owned `RAY_ERROR` in a public C API reference example or the
public-header conformance test is cleaned up with `ray_release`, even though
the public ownership contract says that helper is a no-op for errors and that
the owner must call `ray_error_free`.

Reproduce the census with:

```sh
rg -n -C 12 'ray_release\((result|err)\)' docs/docs/c-api test/test_public_api.c
```

For every match, retain it only when the same example/test has already proved
the released value is an error with `RAY_IS_ERR`. The search covers the whole
public C API reference tree and its public-header conformance test.

## Census

- `docs/docs/c-api/core.md:490-492`, the `RAY_IS_ERR(p)` example releases the
  owned `result` error.
- `docs/docs/c-api/dag.md:373-375`, the `ray_execute` example releases the
  owned `result` error.
- `test/test_public_api.c:580-592`,
  `test_public_get_error_trace_populated` releases the owned `err` error.
- `test/test_public_api.c:601-604`,
  `test_public_get_error_trace_cleared_on_eval` releases the owned `err`
  error.

Four instances meet the project threshold of three.

## Validation

Re-ran the recorded census on 2026-07-22:

```sh
rg -n -C 12 'ray_release\((result|err)\)' docs/docs/c-api test/test_public_api.c
```

All four recorded instances remain eligible: each value is proved to be a
`RAY_ERROR` before cleanup. `include/rayforce.h` documents
`ray_error_free` as the owned-error cleanup and `src/mem/cow.c` confirms that
`ray_release` deliberately returns without freeing errors. The trace returned
by `ray_get_error_trace` is separately owned by the VM, so freeing the error
result does not invalidate either public trace test.


## Root cause

The public error-ownership norm in `include/rayforce.h` was added without a
complete update of the public examples and conformance tests that demonstrate
error cleanup.

## Global strategy

Rung (a): replace all four cleanup calls with `ray_error_free`, then run the
public API test and rebuild the documentation. Re-run the recorded census and
require zero public-contract instances.

## Remediation

Planned in `audit/plans/RP-0002.md` using the accepted rung-(a) global
strategy. No ownership norm changes or conflicts require ratification.

Replaced all four eligible cleanup calls with `ray_error_free`: the
`RAY_IS_ERR` examples in `docs/docs/c-api/core.md` and
`docs/docs/c-api/dag.md`, plus both error-trace cases in
`test/test_public_api.c`.

Self-checks completed on 2026-07-22:

- The recorded census returned only three success-path `ray_release(result)`
  calls guarded by `!RAY_IS_ERR`; zero eligible class instances remain.
- `make test` passed 3638 of 3638 tests with 0 skipped and 0 failed.
- `mkdocs build --strict` completed successfully.


## Verification

Pending independent Verifier review.
