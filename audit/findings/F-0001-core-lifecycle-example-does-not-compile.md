---
id: F-0001
title: Core lifecycle example does not compile against its documented header
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

In `docs/docs/c-api/core.md`, section “Headers” (line 4), the page says:

> Include `<rayforce.h>` for the bulk of the public API. A few internal helpers documented here live in their own headers — in particular `ray_cow` and the `ray_heap_init` / `ray_heap_destroy` lifecycle calls are declared in `"mem/heap.h"`, not in `<rayforce.h>`.

But its “Typical lifecycle pattern” (lines 42–54) contains only:

```c
#include <rayforce.h>

int main(void) {
    ray_heap_init();
    ray_sym_init();
    /* ... use Rayforce API ... */
    ray_sym_destroy();
    ray_heap_destroy();
```

Compiling that pattern as C17 with `cc -std=c17 -Werror -fsyntax-only
-Iinclude` fails with implicit declarations for both heap calls.

The public lifecycle norm in `include/rayforce.h`, section “Runtime + Rayfall
Eval API” (lines 623–664), instead says:

> Embedders build a runtime, optionally restoring a persisted symbol table, then evaluate Rayfall source strings against the global env.

and declares:

```c
ray_runtime_t* ray_runtime_create(int argc, char** argv);
void ray_runtime_destroy(ray_runtime_t* rt);
```

`test/test_public_api.c`, `test_public_runtime_create_with_sym_eval` (lines
406–426), also exercises the public runtime create/destroy lifecycle rather
than the undocumented heap declarations.

## Why this is a defect

The page's canonical lifecycle example fails under the project's C17/Werror
contract and steers embedders away from the lifecycle actually exported by the
public header and exercised by the public API test.

## How to verify the fix

Extract the revised “Typical lifecycle pattern” into a temporary `.c` file and
run `cc -std=c17 -Werror -fsyntax-only -Iinclude <file>`. It must compile using
exactly the headers shown in the example, and its create/destroy pairing must
match a lifecycle declared in `include/rayforce.h` and exercised by
`test/test_public_api.c`.

## Validation

Validated. The quoted documentation and public declarations are still present.
Recompiling the documented pattern with
`cc -std=c17 -Werror -fsyntax-only -Iinclude` fails on implicit declarations
of `ray_heap_init` and `ray_heap_destroy`, while the public header and public API
tests expose the runtime create/destroy lifecycle.

## Census

Pattern: a standalone public C example calls `ray_heap_init` or
`ray_heap_destroy` without including `mem/heap.h`, or presents those internal
calls as the public embedder lifecycle. Searched with
`rg -n 'ray_heap_init|ray_heap_destroy|ray_runtime_create|ray_runtime_destroy'
README.md docs examples` and inspected every containing C example's include
set. The only defective standalone example is the “Typical lifecycle pattern”
in `docs/docs/c-api/core.md`; README and complete DAG examples include
`mem/heap.h`. One instance is below the class threshold.

## Objection


## Remediation

Replaced the “Typical lifecycle pattern” in `docs/docs/c-api/core.md` with the
public `ray_runtime_create`/`ray_runtime_destroy` lifecycle declared by
`rayforce.h`. The example checks runtime creation before use and no longer
calls undeclared internal heap functions.

Self-check: extracted the revised snippet to a temporary C file and ran
`cc -std=c17 -Werror -fsyntax-only -Iinclude`; it exited 0. The full sanitized
suite also passed, 3638/3638.


## Verification
