---
id: F-0006
title: Public API test redeclares runtime functions instead of testing the header
severity: minor
dimension: api-doc-consistency
unit:
  - test/test_public_api.c
  - include/rayforce.h
status: superseded-by-class
class: CF-0004
attempts: 0
pass: P-01
updated: 2026-07-22
---

## Evidence

`test/test_public_api.c` includes the public header at line 27:

```c
#include <rayforce.h>
```

but then manually supplies the public runtime declarations in the file-scope
setup block (lines 34–40):

```c
struct ray_runtime_s;
typedef struct ray_runtime_s ray_runtime_t;
extern ray_runtime_t* ray_runtime_create(int argc, char** argv);
extern void           ray_runtime_destroy(ray_runtime_t* rt);
```

The declarations the test is meant to guard already exist in
`include/rayforce.h` (lines 644–664):

```c
typedef struct ray_runtime_s ray_runtime_t;
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void ray_runtime_destroy(ray_runtime_t* rt);
```

## Why this is a defect

The “public API” test would continue compiling if those runtime declarations
were accidentally removed from the public header, because the test silently
recreates them. It therefore does not guard the public declaration contract it
appears to exercise.

## How to verify the fix

Remove the local runtime type/function redeclarations and compile the public API
test using only `<rayforce.h>` for those symbols. It must pass. As an adversarial
guard check in a temporary copy, hide `ray_runtime_create` from the public
header and confirm compilation then fails.

## Validation

Validated. `test/test_public_api.c` still includes `<rayforce.h>` and then
redeclares `ray_runtime_t`, `ray_runtime_create`, and `ray_runtime_destroy`.
The same declarations remain in the public header, so the local declarations
still mask a header-removal regression.

## Census

The two-stage project search recorded in CF-0004 found sixteen files that
directly include `rayforce.h` and then repeat at least one public runtime type
or function declaration. Sixteen exceeds the threshold, so this finding is
superseded by CF-0004 pending human triage.

## Objection


## Remediation

Superseded by CF-0004 during RP-0001; no point fix was applied.


## Verification
