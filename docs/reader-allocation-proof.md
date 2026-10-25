# Reader-local allocation admission proof (#994)

This is an internal C proof, not a Rayfall builtin or a data reader. It does
not select syntax, adopt numerical defaults, change `format`/`fmt_eq`, or
claim full reader conformance. It starts from upstream `dev` at `e463902f`.

## Boundary

`lang/read_budget.h` exposes an explicit context, not an ambient allocator
hook. It owns charged blocks and bookkeeping until close. Values are borrowed
from the context; retaining a root before close transfers ordinary heap-value
ownership to the caller. Nested string buffers and LIST child references use
the normal refcount rules. Closing after any injected failure releases all
context-owned references and system buffers.

Early `ray_read_release` is conservative: if another reference exists, keep
both the context's ledger reference and its charge. A LIST retaining a child
must not make that child's memory disappear from admission accounting. Release
can be retried after parents/aliases are dropped. Scope close is terminal:
it unconditionally drops the ledger references and rejects further admission.
Only explicit output-root retention followed by close transfers the result
out of the reader scope.

The first constructors are byte strings, I64 vectors (including null flags)
and generic LISTs. They are narrow construction paths whose nested allocations
are visible to the context. Other constructors must not be called inside this
boundary without admitting their transitive allocations. This is not an
assertion that every ordinary engine constructor is budget-aware.

A charged heap block includes its existing header exactly once. Direct blocks
include the prefix and page-rounded mapping. System buffers include their
system header and rounding. The context and record chunks are charged too.
The charge is not process RSS: shared allocator pools, backing-page slack,
runtime bootstrap/infrastructure and thread stacks are outside this owned-block
accounting. No claim is made that this bounds every process allocation.

`ray_alloc_admitted` is explicit. Small blocks use existing size classes.
Direct allocations accept exact-sized cache matches only, so a larger cached
mapping cannot exceed an admitted minimum-size charge. Ordinary `ray_alloc`
retains its existing oversized cache reuse and fallback behavior. The explicit
path does not fall back from a failed direct allocation to an unadmitted buddy
size. Existing allocation tracing is not used as the admission oracle.

System-buffer growth reserves the replacement while the old charge is live.
Failure leaves the old buffer owned and usable. There is no global OOM hook:
`fail_after` injects acquisition failure in one context after reservation,
exercising cancellation of that reservation and subsequent cleanup.

## Focused evidence

Run only the new proof tests:

```sh
make test TEST_FILTER=reader_proof
```

The allocation cases cover cap-1/cap/cap+1, arithmetic overflow, header charging,
string-buffer/atom failure positions, retained-child admission with one/multiple
parent aliases, eventual unique-child reclamation, terminal close, real root
ownership after context close,
resize peaks and preservation of ordinary oversized direct-cache reuse.
ASan/UBSan are enabled by the default test build.

Set `TMPDIR` to an existing disk-backed directory before building/running.
The test driver now places its isolated per-process scratch root there;
its existing scratch-path regression covers the selected root.

## Limits of the proof

The bookkeeping ledger uses small chunks and linear searches. It demonstrates
ownership/admission, not production throughput for millions of allocations.
No parser node/depth/input budgets, full type inventory, DICT/TABLE finalization
or writer/reader acceptance profile is implemented here. Constructor input is
trusted C data, not source text. A complete reader must add its own grammar and
symbol staging without general evaluation.

Keep this proof separate from the
[symbol publication proof](symbol-publication-proof.md). A future reader must
show that every remaining fallible construction step precedes publication.
