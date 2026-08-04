---
id: CF-0004
title: Runtime API is redeclared after including the public header
severity: minor
dimension: api-doc-consistency
unit:
  - fuzz/common.h
  - test/main.c
  - test/test_compile.c
  - test/test_datalog.c
  - test/test_embedding.c
  - test/test_format.c
  - test/test_fused_topk.c
  - test/test_ipc.c
  - test/test_journal.c
  - test/test_lang.c
  - test/test_link.c
  - test/test_public_api.c
  - test/test_repl.c
  - test/test_store.c
  - test/test_term.c
  - test/test_types.c
status: fixed
class: null
members:
  - F-0006
attempts: 0
pass: P-01
created-by: RP-0001
updated: 2026-08-04
---

## Pattern

A C translation unit directly includes `rayforce.h` and then locally repeats
the public `ray_runtime_t`, `ray_runtime_create`, or `ray_runtime_destroy`
declarations. Those repetitions mask removal or drift in the public header.

Reproduce the census with:

```sh
rg -l -0 '#include[[:space:]]*[<"]rayforce\.h[>"]' --glob '*.[ch]' . \
  | xargs -0 rg -n '(struct ray_runtime_s;|typedef struct ray_runtime_s ray_runtime_t;|extern[[:space:]]+ray_runtime_t\*[[:space:]]*ray_runtime_create|extern[[:space:]]+void[[:space:]]+ray_runtime_destroy)'
```

## Census

- `fuzz/common.h:38-40`
- `test/main.c:54-57`
- `test/test_compile.c:40-43`
- `test/test_datalog.c:43`
- `test/test_embedding.c:45-48`
- `test/test_format.c:36-39`
- `test/test_fused_topk.c:51-54`
- `test/test_ipc.c:86-88`
- `test/test_journal.c:46-49`
- `test/test_lang.c:54-57`
- `test/test_link.c:50-53`
- `test/test_public_api.c:37-40`
- `test/test_repl.c:77-80`
- `test/test_store.c:60-62`
- `test/test_term.c:76-79`
- `test/test_types.c:32-35`

Sixteen translation units meet the project threshold of three.

## Validation


## Root cause

The runtime declarations were historically internal and were copied into test
and fuzz harnesses. When they became public, the local compatibility blocks
were not removed and no guard prohibited redeclaring public API symbols.

## Global strategy

Rung (c): remove every redundant public runtime declaration while retaining
declarations of genuinely internal globals such as `__RUNTIME`; compile all
affected test/fuzz targets; and add a lightweight source check that rejects
future runtime API redeclarations in files that include `rayforce.h`.

## Remediation

Removed the redundant public runtime type and lifecycle declarations from all
sixteen test/fuzz files in the census. The files now rely on `<rayforce.h>` for
`ray_runtime_t`, `ray_runtime_create`, and `ray_runtime_destroy`, while keeping
only the genuinely internal `__RUNTIME` extern where the test fixture needs it.

Added `audit/public_runtime_api_not_redeclared` as a lightweight source guard
over the same census so future edits fail the test suite if a file that includes
`rayforce.h` reintroduces one of the public runtime API redeclarations.


## Verification

Completed on 2026-08-04:

- `git diff --check`
- The recorded runtime redeclaration census returns no matches.
- `./rayforce.test -f audit/public_runtime_api_not_redeclared` passed: 1 of 1
  passed, 0 skipped, 0 failed.
- `make test TEST_CORES=2` passed: 3657 of 3658 passed, 1 skipped, 0 failed.
