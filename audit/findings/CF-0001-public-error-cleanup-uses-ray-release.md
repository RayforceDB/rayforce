---
id: CF-0001
title: Public error-cleanup examples use ray_release on owned errors
severity: major
dimension: api-doc-consistency
unit:
  - docs/docs/c-api/core.md
  - docs/docs/c-api/dag.md
  - test/test_public_api.c
status: reported
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


## Root cause

The public error-ownership norm in `include/rayforce.h` was added without a
complete update of the public examples and conformance tests that demonstrate
error cleanup.

## Global strategy

Rung (a): replace all four cleanup calls with `ray_error_free`, then run the
public API test and rebuild the documentation. Re-run the recorded census and
require zero public-contract instances.

## Remediation


## Verification
