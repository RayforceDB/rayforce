---
id: P-01
dimension: api-doc-consistency
units:
  - include/rayforce.h
  - docs/docs/c-api/core.md
  - test/test_public_api.c
status: done
findings:
  - F-0001
  - F-0002
  - F-0003
  - F-0004
  - F-0005
  - F-0006
  - F-0007
updated: 2026-07-22
---

## Charter

Compare the public header, published Core C API reference, and public API test
for declaration, ownership, error, and lifecycle consistency. Stop when all
three units are exhausted, 15 findings are filed, or context is exhausted.

## Coverage

COVERED:

- `include/rayforce.h`, lines 1–770: all public declarations and comments were
  read attentively, with focused comparison of lifecycle, ownership, errors,
  null representation, temporal units, and declarations exercised by the
  public API test.
- `docs/docs/c-api/core.md`, lines 1–517: every section, declaration, example,
  ownership statement, lifecycle instruction, temporal description, and error
  table was compared against the public header and applicable test evidence.
- `test/test_public_api.c`, lines 1–662: all setup/teardown paths, public symbol
  guards, accessor cases, runtime lifecycle tests, interrupt/restricted-mode
  tests, and error cleanup paths were examined.
- A temporary extraction of the documented lifecycle pattern was compiled with
  `cc -std=c17 -Werror -fsyntax-only -Iinclude`; it failed on the two undeclared
  heap lifecycle calls, corroborating F-0001.
- A mechanical `ray_*` symbol comparison and literal-anchor search was run
  across all three units to check omissions and re-find every filed quote.

NOT COVERED:

- Nothing inside the three-unit charter was skipped.
- Implementations outside the charter (including `src/core/runtime.c`,
  `src/mem/cow.c`, and `src/vec/atom.c`) were not opened. Runtime behavior was
  not inferred from those files; findings rely on contradictions among the
  scoped public contracts and executable-test material.
- The full test suite was not run because this pass audits API consistency, not
  implementation correctness; each finding supplies its own focused procedure
  for remediation self-check and independent verification.

## Findings

- F-0001 — Core lifecycle example does not compile against its documented header
- F-0002 — Documentation and public API tests release errors with a no-op helper
- F-0003 — ray_eval_str null-return contract contradicts the public value invariant
- F-0004 — Public null-sentinel contract contradicts its own predicates
- F-0005 — Time atom documentation uses nanoseconds for a millisecond value
- F-0006 — Public API test redeclares runtime functions instead of testing the header
- F-0007 — Public date test labels a different epoch than the API reference

## Handoff

No leftover P-01 scope and no split pass. All seven findings are `reported` and
await human triage. P-02 remains the next queued audit pass.
