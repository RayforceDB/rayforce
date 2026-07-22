---
id: F-0007
title: Public date test labels a different epoch than the API reference
severity: minor
dimension: api-doc-consistency
unit:
  - docs/docs/c-api/core.md
  - test/test_public_api.c
status: fixed
class: null
attempts: 0
pass: P-01
updated: 2026-07-22
---

## Evidence

`docs/docs/c-api/core.md`, section “ray_date” (lines 189–195), defines the norm:

> Creates a date atom. The value is an integer representing days since epoch (2000-01-01).

`test/test_public_api.c`, `test_public_vec_get_i64_date` (lines 260–268), labels
the raw values using a Unix epoch instead:

```c
int32_t xs[] = { 0, 8766, 19724 };  /* 1970.01.01, 1994.01.01, 2024.01.01 */
```

The two passages are an N13 internal-consistency contradiction: in the API
reference, day zero is 2000-01-01; in the public API test comment, day zero is
1970-01-01.

## Why this is a defect

The accessor test's misleading semantic labels can cause future tests and API
examples to encode dates against the wrong epoch, even though the current test
only asserts raw integer round-trips.

## How to verify the fix

Search both units for the quoted epoch/date labels and confirm they name one
epoch consistently. Add or run a focused semantic test in which day zero is
formatted or otherwise converted to the documented calendar date, rather than
only round-tripped as an integer.

## Validation

Validated. The contradictory 1970 and 2000 labels remain. The calendar helpers
and temporal implementation use 2000-01-01 as the Rayforce epoch, so the API
reference is correct and the public test's inline calendar labels are stale.

## Census

Pattern: a `RAY_DATE` raw-value comment or contract labels day zero as 1970
rather than 2000-01-01. Searched date contracts and semantic tests with
`rg -n -i '(RAY_DATE|date atom|date value|date).*\b(1970|2000-01-01|days since|epoch)|\b1970\.01\.01'
include src docs test README.md examples fuzz bench`. Only
`test/test_public_api.c:264` uses the 1970 label; implementation, API reference,
and semantic tests consistently use 2000-01-01. One instance is below
threshold.

## Objection


## Remediation

Corrected the calendar labels beside the raw values in
`test_public_vec_get_i64_date` to the 2000-01-01 epoch: day 0 is 2000.01.01,
day 8766 is 2024.01.01, and day 19724 is 2054.01.01. Added a public semantic
check that formats `ray_date(0)` and requires `2000.01.01`, while retaining all
raw accessor assertions.

Self-check: the stale 1970/1994 labels are absent, the focused public DATE test
passed, and `make test` passed 3638/3638 under ASan/UBSan.


## Verification
