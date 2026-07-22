---
id: F-0004
title: Public null-sentinel contract contradicts its own predicates
severity: major
dimension: api-doc-consistency
unit: include/rayforce.h
status: superseded-by-class
class: CF-0003
attempts: 0
pass: P-01
updated: 2026-07-22
---

## Evidence

`include/rayforce.h`, section “Null Sentinel Values” (lines 438–447), declares:

> SYM null = sym ID 0; STR null = empty string (length 0); BOOL and U8 are non-nullable.

and defines `NULL_F32`:

```c
#define NULL_F32  ((float)__builtin_nanf(""))
```

The public `ray_atom_is_null_fn` contract and implementation directly below it
(lines 449–488) instead say:

> types without a sentinel (BOOL/U8/F32) fall back to the aux[0]&1 bit written by ray_typed_null.

and:

> SYM has no null — sym 0 is the empty symbol ' (a value) ... A SYM atom is never null.

> STR has no null distinct from "" ... A STR atom is never null — the empty string is a value.

The function also has a dedicated `RAY_F32` NaN branch before the fallback.

## Why this is a defect

The same public section assigns incompatible null encodings to SYM, STR, BOOL,
U8, and F32. Callers using the declared sentinels can therefore disagree with
the header's own public predicate about whether a value is null.

## How to verify the fix

For every scalar type named in the conflicting comments, construct its ordinary
zero/empty value and `ray_typed_null(type)`, then assert `RAY_ATOM_IS_NULL` on
both. The test results, sentinel definitions, and all comments in the section
must describe the same matrix with no exception left implicit.

## Validation

Validated. The conflicting comments and `NULL_F32` definition remain.
`ray_typed_null` and `ray_atom_is_null_fn` confirm that F32 uses NaN, BOOL/U8
use the aux bit, and SYM/STR ordinary empty values are not null. Existing tests
cover numeric sentinels but not the full public matrix named by the finding.

## Census

A project-wide search for statements assigning null status to empty SYM/STR
values or denying F32's sentinel found fifteen conflicting statements across
the public header, implementation comments, documentation, and tests. The full
census and ratification requirement are recorded in CF-0003. This finding is
superseded pending triage of that class finding.

## Objection


## Remediation

Superseded by CF-0003 during RP-0001; no point fix was applied.


## Verification
