# Whole-document value-symbol publication (#994)

`read-value` publishes the symbols it staged through this entry point.

`ray_sym_publish_values` is a separate internal C entry point, not a change to
existing partial-batch intern semantics. It uses the explicit allocation
context in the [allocation proof](reader-allocation-proof.md). The caller
provides staged names, output-ID storage and a host total-symbol-count ceiling.

## Transaction

Input bytes are copied and deduplicated outside the symbol lock. Inspection
under the lock rechecks names already committed by other callers and the host
ceiling. A partially materialized lazy global table is explicitly rejected:
materializing it during inspection would add hidden allocations and state
changes. Support for that state remains future work.

All candidate strings, replacement hash/strings/sidecar arrays and arena
storage are allocated and charged before changing any global table pointer,
name or ID. Replacement arrays are built in private storage. When the symbol
arena's head chunk has room for every new name, nothing is allocated for them;
otherwise one new chunk, sized for the names plus a default chunk of slack, is
allocated and charged here. Any preparation failure frees this work; no capacity
expansion is retained by the global table.

Commit swaps only prepared storage, adopts the prepared chunk (empty, as the new
head, when one was needed), bump-allocates the name atoms from the head chunk,
and writes the output IDs. The head chunk's room was checked under the lock, so
it has no allocator call or failure return remaining. The head chunk stays
usable by later interns: a loop that alternates publications with ordinary
interns does not strand a chunk per iteration. Global symbols are published as values: no dotted-segment creation
is performed. Existing name-resolution interning may populate dotted metadata
later through its unchanged ordinary path.

This is name-set and capacity failure atomicity under the existing symbol lock.
Parsing does not hold the lock. Preparation still allocates/rehashes/copies
under the lock after inspection; lock duration and staging overhead need cost
work before treating this as a production decoder. Host lifetime/admission
policy remains separate from the reader's allocation limit. The ceiling applies
to this publication; ordinary existing intern entry points are not retrofitted
with a global quota. A host requiring that quota must coordinate all callers.
A chunk is adopted only when the head chunk is too full, so chunk growth follows
the arena's own policy rather than the number of publications.

## Focused evidence

The five `reader_proof/publication_*` tests demonstrate:

- Failure injection at every acquisition step for a 300-name batch forcing
  hash and sidecar growth: unchanged names/IDs/output storage, a byte-identical
  global table descriptor, preserved existing dotted metadata, and system
  allocation counters back at baseline.
- Deduplication, existing-name reuse and host-ceiling rejection before commit.
- Actual concurrent ordinary interning and transaction publication agreeing on
  IDs, without duplicate names or exceeding the shared host ceiling.
- Publication allocation cap-1/cap/cap+1 with explicit peak accounting.
- A result LIST and private symbol slot allocated before publication, followed
  only by a nonallocating ID fixup, root retain and context cleanup. The root
  survives normal ownership transfer. No fallible result-building follows.

The LIST example is deliberately narrow. It does not prove that DICT/TABLE
building, parser validation or full reader publication is nonfailing. Those
must be implemented and tested before claiming a complete data reader. Existing
single-name atomic interning and arena regressions are also run separately.

## Proposed review boundary

No builtin spelling, grammar, default limits or compatibility commitment is
introduced. The entry point is explicit, existing partial-batch callers remain
unchanged, and no rollback is claimed for arbitrary work a caller performs
after a successful publication. The future reader must prebuild all fallible
result/fixup structures and then use only nonfailing finalization.

The focused concurrent case also passes the ThreadSanitizer build:

```sh
make tsan-test TSAN_FILTER=reader_proof/publication_concurrent TSAN_CORES=2
```
