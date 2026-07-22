---
id: F-0005
title: Time atom documentation uses nanoseconds for a millisecond value
severity: major
dimension: api-doc-consistency
unit:
  - docs/docs/c-api/core.md
  - include/rayforce.h
  - test/test_public_api.c
status: fixed
class: null
attempts: 0
pass: P-01
updated: 2026-07-22
---

## Evidence

`docs/docs/c-api/core.md`, section “ray_time” (lines 197–203), specifies:

> Creates a time atom. The value is nanoseconds since midnight.

`test/test_public_api.c`, `test_public_vec_get_i64_time` (lines 273–280), uses
values explicitly labelled as wall-clock times:

```c
int32_t xs[] = { 0, 43200000, 86399000 };  /* 00:00:00.000, 12:00:00.000, 23:59:59.000 */
```

Those values are milliseconds since midnight, not nanoseconds. The same test's
section comment (lines 256–258) and the public null predicate in
`include/rayforce.h` (lines 465–467) also establish that `RAY_TIME` uses a
32-bit `i32` representation; a full day in nanoseconds cannot fit in that
representation.

## Why this is a defect

The public constructor documentation scales every time value by the wrong
factor of one million. An embedder following it cannot represent ordinary
times in the declared 32-bit storage and will create incorrect values.

## How to verify the fix

Re-read the `ray_time` section and run a public API test constructing noon and
23:59:59.000 with the documented unit. Expected values must match the existing
32-bit executable contract (`43200000` and `86399000` for milliseconds), and
the documentation must name that unit exactly.

## Validation

Validated. The API page still says nanoseconds. `src/lang/format.c` converts the
stored `int32_t` value by dividing by 1000, `src/ops/temporal.c` names the unit
as milliseconds, and the public API test uses `43200000` for noon. The runtime
contract is milliseconds.

## Census

Pattern: a contract for `RAY_TIME` or `ray_time` names a unit other than
milliseconds since midnight. Searched with
`rg -n -i '(RAY_TIME|time atom|time-of-day).*\b(nanosecond|microsecond|millisecond)|\b(nanosecond|microsecond|millisecond)s? since midnight'
include src docs test README.md examples fuzz bench`. Only
`docs/docs/c-api/core.md:199` says nanoseconds; the type guide, data-type guide,
syntax reference, implementation comments, and tests consistently say
milliseconds. One instance is below threshold.

## Objection


## Remediation

Changed `docs/docs/c-api/core.md`, section `ray_time`, to specify milliseconds
since midnight. Strengthened `test_public_vec_get_i64_time` in
`test/test_public_api.c` by constructing `ray_time(43200000)` and asserting its
public formatted value is `12:00:00.000`.

Self-check: the stale “nanoseconds since midnight” text is absent, the focused
public TIME test passed, and `make test` passed 3638/3638 under ASan/UBSan.


## Verification
