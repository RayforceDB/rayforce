# Universal DAG VM — Lazy-Chain Discipline & Idiom Rewriter

**Status:** Draft (revision 2 — universal-DAG framing)
**Date:** 2026-05-04
**Author:** Anton (with Claude)
**Supersedes:** revision 1 (single-pass DAG idiom rewriter, 2026-05-04)

---

## The principle

**One universal DAG VM. Call site is invisible.**

Whether the user writes `(count V)` at the REPL, `(count (distinct V))`
in a chain, or `(select count(distinct v) from T)` in a query — every
expression that *can* be expressed in the DAG vocabulary builds the
same DAG, runs through the same optimizer, executes on the same
`ray_execute` codepath. The eager `*_fn` surface functions are not a
parallel evaluator; they are the **entry points** that decide whether
to extend an existing lazy chain or start a fresh one. They never
materialise unless they are the materialisation boundary themselves.

## Why revision 2

Revision 1 designed a DAG idiom-rewrite pass under the assumption
that shapes like `(count (distinct V))` already form chains that the
optimizer sees. They don't. An audit of the `ray_lazy_*` machinery
showed:

- **`ray_lazy_wrap` is called in 3 places, all in `agg.c`, and each
  is immediately followed by `ray_lazy_materialize` on the same line**
  (`agg.c:90, 225, 254`). No producer ever actually returns lazy.
- **`ray_lazy_append` is called in 7 places**, all in `agg.c` (lines
  110/177/194/217/246/275/314), correctly shaped — but they're dead
  code in practice because no producer upstream emits lazy.
- **`distinct`, `asc`, `desc`, `reverse` lack DAG opcodes entirely**
  (`grep "OP_DISTINCT\|OP_ASC\|OP_DESC\|OP_REVERSE" src/ops/ops.h`
  returns zero hits). Their `*_fn` surface functions
  (`collection.c:701, 1710`, `sort.c:3347, 3358`) eagerly materialise
  any lazy input and operate on raw bytes.
- **`ray_lazy_materialize` does not call `ray_optimize`**
  (`graph.c:1711` jumps straight to `ray_execute`). The optimizer
  runs only inside `query.c`'s select compiler — not on REPL chains.
- **IPC, journal, and serde paths have no `ray_lazy_materialize`
  calls**. They work today only because no producer ever returns
  lazy.

So the original "Phase 1 = lift four ops; Phase 2 = idiom rewriter"
framing was incomplete. The honest framing is one principle with three
mechanical consequences. This revision restructures around that.

## Non-goals

- **Surface-language sugar.** Adding `count_distinct` / `countd` as a
  Rayfall-callable name is out of scope. The whole point is that the
  user keeps writing `(count (distinct V))` and the system gets fast.
- **Grouped aggregation rewrites.** `OP_GROUP`'s `agg_ins[a]`
  sub-DAGs are out of scope; group has its own optimised path.
- **A pattern DSL or e-graph.** A static C table is sufficient at
  the catalog size we anticipate (≤ ~30 idioms).
- **Lifting every quietly-eager `*_fn`.** Only the four with concrete
  rewrite payoff in the day-1 idiom catalog (`distinct/asc/desc/
  reverse`). The wider audit informs follow-up work but isn't part
  of v1.

## Architecture

The change has three layers. They land together; partial landings
leave the codebase in a worse state than today (a producer that
returns lazy with no boundary materialisation downstream would
crash any consumer that reads raw bytes).

### Layer A — Producers return lazy

Every supported `*_fn` follows this template:

```c
ray_t* ray_X_fn(ray_t* x) {
    /* Validate without forcing materialisation. ray_is_lazy()
       and the type tag are concrete bits on the lazy header. */
    if (!x || RAY_IS_ERR(x)) return x;
    if (ray_is_atom(x)) { /* op-specific atom handling */ }

    /* Extend an existing chain. */
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_X);

    /* Start a fresh chain. Wrap the input as a const-vec node,
       chain OP_X on top, return lazy. Do NOT materialize. */
    ray_graph_t* g  = ray_graph_new();
    ray_op_t*    in = ray_const_vec(g, x);
    ray_op_t*    op = ray_X_op(g, in);
    return ray_lazy_wrap(g, op);
}
```

**Rule:** an `*_fn` only materialises if it itself sits at a
materialisation boundary (Layer B), or if it needs raw bytes for an
op the DAG can't yet express. The `wrap-and-materialize-immediately`
shorthand currently in `agg.c:90, 225, 254` is the bug; it becomes
`wrap-and-return-lazy`.

**Atoms stay concrete.** Negative-type-tag values (atoms) are scalar
literals. They never enter chains. The `ray_is_atom(x)` early-exit
in each `*_fn` covers that case.

**Existing aggregation `*_fn`s already extend correctly** (`agg.c:110,
177, 194, 217, 246, 275, 314`). They just need their leaf-case
branches changed from `wrap+materialize` to `wrap+return_lazy`.

### Layer B — Boundaries materialise (and optimise)

A lazy value transitions to a concrete vector at exactly the points
where bytes leave the universal VM. Each of these sites must call
`ray_lazy_materialize` on any lazy input. They split into two groups:

**Already wired (no change needed):**

| Site                          | File:line                                |
|-------------------------------|------------------------------------------|
| REPL output                   | `src/app/repl.c:731, 1226`               |
| Format / print                | `src/lang/format.c:1053`                 |
| `set` (global env bind)       | `src/lang/eval.c:1119`                   |
| `let` (local env bind)        | `src/lang/eval.c:1137`                   |
| `if` truthiness test          | `src/lang/eval.c:1151`                   |
| Comparison kernels            | `src/ops/cmp.c:271`                      |
| Builtins arg coercion         | `src/ops/builtins.c:81, 167, 232, 261, 288, 310, 347` |
| Collection-op arg coercion    | `src/ops/collection.c:340, 406, 454, 494, 495, 609, 800, 801, 978, 1058, 1120, 1178, 1496, 1619, 1970, 2024` |

**Missing — must be added in v1:**

| Site                          | Where                                     | Reason                                                              |
|-------------------------------|-------------------------------------------|---------------------------------------------------------------------|
| IPC send (sync + async)       | `src/core/ipc.c:1027` (sync), `:1091` (async) | Wire frame must contain concrete bytes.                             |
| Journal write                 | `src/store/journal.c` (`ray_journal_write_bytes` callers) | Persisted log must be replayable; can't store deferred computations. |
| Splay/serde write             | `src/store/serde.c` (top-level write entry points)        | Same as journal.                                                    |

**Materialise also runs the optimiser.** Today
`ray_lazy_materialize` (`graph.c:1706-1719`) calls `ray_execute`
directly, skipping the optimizer. Change:

```c
ray_t* ray_lazy_materialize(ray_t* val) {
    if (!ray_is_lazy(val)) return val;
    ray_graph_t* g  = RAY_LAZY_GRAPH(val);
    ray_op_t*    op = RAY_LAZY_OP(val);
    op              = ray_optimize(g, op);    /* NEW LINE */
    ray_t* result   = ray_execute(g, op);
    ray_graph_free(g);
    RAY_LAZY_GRAPH(val) = NULL;
    ray_release(val);
    return result;
}
```

Without this, even with chains forming correctly, the optimizer
(and our new idiom pass) would never run on REPL-built chains —
they'd execute as written.

### Layer C — Lift four ops into the DAG

`distinct`, `asc`, `desc`, `reverse` join the universal VM. Each
gets:

1. **A new opcode** in `src/ops/ops.h`.
2. **A DAG builder** `ray_X_op(g, in)` in `src/ops/graph.c` (the
   `_op` suffix avoids colliding with the existing `*_fn` surface
   names).
3. **An executor case** in `src/ops/exec.c`'s opcode dispatch,
   delegating to a refactored core (`exec_distinct`, `exec_asc`,
   `exec_desc`, `exec_reverse`) that takes a `ray_op_t* + ray_t*` and
   returns `ray_t*`. The implementation reuses the existing
   `ray_*_fn` body; only the entry-point shape changes.
4. **A dump entry** in `src/ops/dump.c`'s `ray_opcode_name`.
5. **A lazy-append type rule** in `ray_lazy_append`
   (`src/ops/graph.c:1677`): all four preserve input type.
6. **The `*_fn` surface function** rewritten to follow the Layer A
   template — extend if lazy, wrap-lazy if concrete.

This expands the DAG vocabulary by 4 opcodes
(highest currently used = `OP_KNN_RERANK = 103`; the new ones
slot into the next free range).

### Layer D — Idiom rewriter (the original spec, unchanged design)

A new pass `pass_idiom_rewrite` inserted as **Pass 3** in
`ray_optimize` (`src/ops/opt.c:2013`), between constant folding and
SIP. New files `src/ops/idiom.{c,h}`. Table-driven, ~6 rows on day
one, no new opcodes (every replacement maps to an existing `OP_*`
including the four added in Layer C).

This layer is unchanged from revision 1 and is documented in detail
below (§Idiom rewriter — design retained from rev 1).

### Why all four layers must land together

- **A without B:** producers emit lazy, consumers read raw bytes
  without materialising → segfault.
- **A + B without optimiser-in-materialise:** chains form, optimizer
  never runs, no win except what the executor extracts incidentally
  from chained execution. The whole architectural pivot delivers
  nothing observable.
- **A + B + opt without C:** `(count (distinct V))` still doesn't
  chain because `distinct` materialises. The headline rewrite never
  fires.
- **A + B + opt + C without D:** chain is fully visible to the
  optimizer, but no rewrite recognises the count-distinct shape →
  executor runs `OP_DISTINCT` (materialises a deduped vector) then
  `OP_COUNT` (reads len). Functionally correct, no perf win.

So the project is a single coherent change. It can be staged across
multiple PRs internally, but the spec/plan treats it as one unit.

## Idiom rewriter — design retained from rev 1

### Insertion

`pass_idiom_rewrite` becomes Pass 3:

```
Pass 1:  type inference
Pass 2:  constant folding
Pass 3:  idiom rewrite           ← NEW
Pass 4:  SIP
Pass 5:  factorize
Pass 6:  predicate pushdown
Pass 7:  filter reorder
Pass 8:  projection pushdown
Pass 9:  partition pruning
Pass 10: fusion
Pass 11: DCE
```

After const-fold so the matcher never has to handle a partially
folded `OP_ASC` over a literal vector. Before everything else so
later passes see the simplified shape. DCE at the end sweeps any
node the rewriter marks `OP_FLAG_DEAD`.

### Files

- `src/ops/idiom.h` — public interface (`ray_idiom_pass`, `ray_idiom_t`).
- `src/ops/idiom.c` — pass body, dispatch index, idiom table,
  per-row predicates and rewrites.

Both compile via `Makefile`'s `wildcard src/*/*.c`. No manifest edit.

### Data structures

```c
/* src/ops/idiom.h */
typedef bool      (*ray_idiom_pre_t)(ray_graph_t* g, ray_op_t* node);
typedef ray_op_t* (*ray_idiom_rw_t) (ray_graph_t* g, ray_op_t* node);

typedef struct {
    uint16_t          root_op;
    uint16_t          child0_op;
    ray_idiom_pre_t   pre;       /* NULL = always */
    ray_idiom_rw_t    rewrite;   /* returns replacement node, or NULL */
    const char*       name;
} ray_idiom_t;

extern const ray_idiom_t ray_idioms[];
extern const int         ray_idioms_count;

void ray_idiom_pass(ray_graph_t* g, ray_op_t* root);
```

Static table in `idiom.c`. No registration API.

### Dispatch index

```c
/* Sized generously above the highest currently-defined opcode
 * (OP_KNN_RERANK = 103, plus 4 new opcodes from Layer C). A
 * _Static_assert in idiom.c guards against silent overflow if
 * a future opcode pushes past the cap. */
#define RAY_IDIOM_OPCODE_CAP 128
#define RAY_IDIOM_MAX_ROWS    64

static int8_t first_idiom[RAY_IDIOM_OPCODE_CAP]; /* -1 = no idioms */
static int8_t next_idiom [RAY_IDIOM_MAX_ROWS];   /* chain link     */
static bool   index_built;

_Static_assert(/* highest used root_op */ < RAY_IDIOM_OPCODE_CAP,
               "idiom dispatch index too small");
```

Built once per process, ~192 bytes BSS. Per-node lookup is O(1)
bucket head + chain walk.

### Walk + replacement

Single post-order traversal (no fixpoint — none of v1's idioms
cascade). For each live node: bucket-lookup by `node->opcode`, walk
the chain, check `inputs[0]->opcode == row->child0_op`, run
`row->pre` if present, call `row->rewrite`. On success:

1. Allocate the replacement via `graph_alloc_node_opt`
   (`opt.c:1027`).
2. `redirect_consumers(g, n->id, replacement->id)` (`opt.c:1312`).
3. `n->flags |= OP_FLAG_DEAD`.

Same convention as `factorize_pass` / `pass_predicate_pushdown`.
Never mutate `node->opcode` in place. The orphaned child becomes
unreferenced and `pass_dce` (Pass 11) sweeps it.

Ext-bearing nodes (`OP_GROUP/OP_SORT/OP_JOIN/OP_WINDOW/
OP_WINDOW_JOIN/OP_SELECT`) are skipped as roots in v1.

### Day-1 catalog (6 rows)

| # | Pattern                  | Replacement              | Precondition                                  | Notes                                                                                            |
|---|--------------------------|--------------------------|-----------------------------------------------|--------------------------------------------------------------------------------------------------|
| 1 | `(count (distinct v))`   | `OP_COUNT_DISTINCT(v)`   | none                                          | Hash-set executor at `group.c:158`. Headline rewrite.                                            |
| 2 | `(count (asc v))`        | `OP_COUNT(v)`            | none                                          | Sort doesn't change cardinality.                                                                 |
| 3 | `(count (desc v))`       | `OP_COUNT(v)`            | none                                          | Same.                                                                                            |
| 4 | `(count (reverse v))`    | `OP_COUNT(v)`            | none                                          | Same.                                                                                            |
| 5 | `(first (asc v))`        | `OP_MIN(v)`              | `!(v->attrs & RAY_ATTR_HAS_NULLS)` statically | Without gate, semantics diverge under nulls. `xasc` puts nulls first; `OP_MIN` skips nulls.      |
| 6 | `(last (asc v))`         | `OP_MAX(v)`              | `!(v->attrs & RAY_ATTR_HAS_NULLS)` statically | Same nulls reasoning.                                                                            |

The precondition for rows 5 & 6 returns true only when the input's
null state is statically known clean (e.g. `OP_SCAN` of a column
header with `RAY_ATTR_HAS_NULLS == 0`, or a constant vector with no
nulls). For lazy/computed inputs whose null state isn't tracked,
return false → slow form runs. False negatives are acceptable; false
positives are not.

### Tests

- `test/rfl/ops/idiom.rfl` — behavioural equivalence per row (empty,
  single, all-equal, mixed, null-bearing for 5/6, boundaries).
- No structural-shape test in v1 — dump-string assertions are too
  brittle for opcode renames.

Test harness: per the layout memo, `.rfl` files auto-discovered by
`test/main.c`. Run `make test` or `./rayforce.test -f idiom`.

### Profiling

```c
ray_profile_span_start("idiom");
/* pass body */
ray_profile_span_end("idiom");
ray_profile_tick("idiom rewrite");
```

Same convention every other pass uses (`opt.c:2018-2059`). The
`name` field on each row is reserved for future `--debug-opt` output.

## Edge cases & failure modes

| Case                                        | Behaviour                                                                                              |
|---------------------------------------------|--------------------------------------------------------------------------------------------------------|
| `node->inputs[0] == NULL`                   | Skip the row.                                                                                          |
| Rewrite callback returns `NULL`             | Treat as "doesn't apply"; continue chain.                                                              |
| `graph_alloc_node_opt` returns `NULL` (OOM) | Rewrite returns `NULL`; original shape executes. Optimisation is best-effort.                          |
| Multiple rows match                         | First-match-wins. Document by ordering rows most-specific to least-specific.                           |
| Node already `OP_FLAG_DEAD`                 | Walk skips dead nodes.                                                                                 |
| Lazy materialisation under OOM              | Standard error propagation; lazy header is freed in `ray_lazy_materialize`'s release branch as today.  |
| Lazy chain refers to a freed graph          | Cannot happen — `ray_lazy_wrap` retains the graph; `ray_lazy_materialize` frees it after execute.      |
| `ray_optimize` mutates `op` (e.g. predicate pushdown changes root) | `ray_optimize` already returns the new root; we re-assign `op` before passing to `ray_execute`. Existing call sites in `query.c:322` etc. follow the same pattern. |

## Audit deliverables (v1 includes the audit; cleanup beyond the four ops is follow-up)

- **Producer audit** — list every `*_fn` in `eval.c`'s
  `register_unary` / `register_binary` table that does eager work on
  vector inputs. Categorise: (a) lazy-correct (already extends),
  (b) needs lift (DAG opcode missing — v1 lifts 4 of these),
  (c) intentionally eager (special form, side-effecting, or no
  meaningful DAG representation — keep eager).
- **Consumer audit** — `grep ray_data\|->len\|AS_I64\|AS_F64` for
  call sites that read raw bytes without a preceding
  `ray_lazy_materialize` or `ray_is_lazy` guard. Most are inside
  executors (running on already-materialised data — fine); flag any
  in `*_fn` surface paths.
- **Boundary audit** — confirm IPC, journal, serde top-level write
  paths gain `ray_lazy_materialize` calls in v1.

## Out of scope (deferred)

- Lifting more `*_fn`s into the DAG (the audit names them; the work
  is mechanical follow-up).
- New executor opcodes for richer rewrites (e.g.
  `(count (where pred …)) → OP_COUNT_WHERE`,
  `(sum (* c v)) → c * sum(v)`).
- Pattern DSL.
- Surface-language sugar for `count_distinct`.
- Grouped-form rewrites inside `OP_GROUP`'s `agg_ins[]`.

## Open questions

None at design time. Layered framing closes the original spec's
implicit assumption gap.
