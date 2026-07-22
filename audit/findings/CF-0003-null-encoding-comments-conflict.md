---
id: CF-0003
title: Null-encoding comments conflict across scalar and vector surfaces
severity: major
dimension: api-doc-consistency
unit:
  - include/rayforce.h
  - src/vec/atom.c
  - src/vec/vec.c
  - src/table/domain.h
  - src/ops/pivot.c
  - docs/docs/namespaces/csv.md
  - test/stress_store.c
  - test/test_dict.c
  - test/test_format.c
  - test/test_str.c
  - test/rfl/integration/null.rfl
  - test/rfl/strop/split.rfl
status: planned
class: null
members:
  - F-0004
attempts: 0
pass: P-01
created-by: RP-0001
updated: 2026-07-22
---

## Pattern

A comment or API-facing statement assigns null status to empty SYM/STR values,
or says F32 has no sentinel, while the public atom/vector predicates and
current focused tests say SYM/STR empty values are ordinary values and F32
uses NaN.

Reproduce the candidate census with:

```sh
rg -n -i '(SYM null\s*=|STR null\s*=|empty string IS.*null|empty symbol.*null|BOOL/U8/F32|types without a sentinel|global id 0.*null|id 0 is the SYM null)' \
  include src docs test README.md examples fuzz bench
```

Classify each candidate against `ray_atom_is_null_fn` in
`include/rayforce.h`, `ray_vec_is_null` in `src/vec/vec.c`, and the explicit
`nil?` expectations in `test/rfl/null/sym_no_null.rfl`.

## Census

- `include/rayforce.h:441-442` assigns SYM id 0 and empty STR as null.
- `include/rayforce.h:451-452` incorrectly groups F32 with types lacking a
  sentinel.
- `src/vec/atom.c:185-186` repeats the F32 no-sentinel claim.
- `src/table/domain.h:50-51` calls global SYM id 0 null.
- `src/ops/pivot.c:1268-1269` calls SYM id 0 the SYM null.
- `src/vec/vec.c:1156-1157` calls an empty STR descriptor a STR null.
- `src/vec/vec.c:1200` repeats “STR null = empty string”.
- `docs/docs/namespaces/csv.md:25` says empty strings are read as null and
  marked `RAY_ATTR_HAS_NULLS`.
- `test/stress_store.c:118` labels the empty symbol as a SYM null.
- `test/test_dict.c:1177-1179` says empty STR is a null atom.
- `test/test_dict.c:1214` repeats the STR-null/empty-string conflation.
- `test/test_format.c:751-753` calls the empty string and “null string” the
  same null-like value, despite the predicate returning false.
- `test/test_str.c:1830` says a STR null is stored as an empty string.
- `test/rfl/integration/null.rfl:8` says “STR null = empty string” immediately
  before an expectation that `(nil? "")` is false.
- `test/rfl/strop/split.rfl:4-5` says empty string is the STR null immediately
  before an expectation that the resulting empty value is not null.

Fifteen instances meet the project threshold of three.

## Validation

Re-ran the recorded candidate census on 2026-07-22 and manually classified
each result against `ray_atom_is_null_fn`, `ray_vec_is_null`,
`ray_vec_set_null_checked`, and the focused `nil?` expectations. All fifteen
recorded conflicting statements remain in their cited locations. The search
also returns correct counterexamples that explicitly say empty SYM/STR values
are not null; those are classification references, not defects.

The current executable behavior is internally consistent enough to expose the
norm choice but not to authorize it:

- F32 atoms and vectors use NaN as a null sentinel.
- SYM and STR atoms have no null distinct from their empty values;
  `ray_typed_null` collapses to the ordinary empty value and
  `RAY_ATOM_IS_NULL` returns false.
- SYM and STR vectors are likewise treated as non-nullable by
  `ray_vec_is_null`; missing inputs collapse to empty payloads without
  `RAY_ATTR_HAS_NULLS`.
- BOOL and U8 typed-null atoms use the auxiliary null bit, while BOOL and U8
  vectors reject null insertion.

The conflict is therefore substantive and the finding is validated. Because
the Global strategy is rung (b) and explicitly requires the norm owner to
choose the cross-surface matrix, validation does not ratify either direction.


## Root cause

The project lacks one ratified nullability matrix that distinguishes typed-null
construction, payload sentinels, empty-value collapse, and the public null
predicates. Comments from incompatible historical models coexist.

## Global strategy

Proposed rung (b): first obtain norm-owner ratification for a single per-type
matrix, because the current normative and executable surfaces conflict. Then
fix all census instances and write the ratified matrix into the public API
contract. The norm ratification gate applies: no global edit should execute
until triage records which semantics own SYM/STR empty values and F32.

## Remediation

Planned in `audit/plans/RP-0004.md`. Execution is paused at the mandatory
norm-ratification gate pending the human norm owner's explicit selection of
the SYM/STR and F32 cross-surface semantics.


## Verification
