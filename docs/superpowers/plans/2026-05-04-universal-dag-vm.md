# Universal DAG VM — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `(count (distinct V))` go through the universal DAG VM. By extension: every supported expression builds a DAG regardless of call site, the optimizer (with a new idiom-rewrite pass) sees full chains, and the executor runs once at materialisation boundaries.

**Architecture:** Four layers, landed in dependency order — (B) close the missing materialisation boundaries first so producers returning lazy don't crash anything, (A) flip the existing aggregations from `wrap+materialize` to `wrap+lazy`, (C) lift `distinct/asc/desc/reverse` into the DAG vocabulary, (D) add the table-driven idiom-rewrite pass to `ray_optimize`.

**Tech Stack:** C17, `wildcard src/*/*.c` Makefile, sanitiser-enabled debug builds (`make test`), per-test `.rfl` files under `test/rfl/<group>/<name>.rfl` auto-discovered by `test/main.c`.

**Spec:** `docs/superpowers/specs/2026-05-04-dag-idiom-rewrite-design.md`

---

## File map

| File                          | Change                                                                                         |
|-------------------------------|------------------------------------------------------------------------------------------------|
| `src/ops/graph.c:1706-1719`   | `ray_lazy_materialize` calls `ray_optimize(g, op)` before `ray_execute`.                       |
| `src/core/ipc.c:1135, 1199`   | `ray_ipc_send` / `ray_ipc_send_async` materialise lazy `msg` before serialising.               |
| `src/store/serde.c`           | `ray_obj_save` (declared at `serde.h:78`) materialises lazy `obj` before serialising.          |
| `src/ops/agg.c:85-91`         | `AGG_VEC_VIA_DAG` macro: drop the `ray_lazy_materialize` wrap.                                 |
| `src/ops/ops.h`               | Add `OP_DISTINCT`, `OP_ASC`, `OP_DESC`, `OP_REVERSE` (next free range above `OP_KNN_RERANK=103`). |
| `src/ops/graph.c`             | Add `ray_distinct_op`, `ray_asc_op`, `ray_desc_op`, `ray_reverse_op` builders + `ray_lazy_append` type rules. |
| `src/ops/exec.c`              | Dispatch cases for the four new opcodes → `exec_distinct`/`_asc`/`_desc`/`_reverse`.           |
| `src/ops/dump.c`              | `ray_opcode_name` entries for the four new opcodes.                                            |
| `src/ops/collection.c:700`    | `ray_distinct_fn` rewritten to lazy-producer template.                                         |
| `src/ops/collection.c:1710`   | `ray_reverse_fn` rewritten.                                                                    |
| `src/ops/sort.c:3347, 3358`   | `ray_asc_fn` / `ray_desc_fn` rewritten.                                                        |
| `src/ops/idiom.h` (new)       | Public interface (`ray_idiom_pass`, `ray_idiom_t`).                                            |
| `src/ops/idiom.c` (new)       | Pass body, dispatch index, idiom table, six rewrite functions.                                 |
| `src/ops/opt.c:2013`          | Insert `ray_idiom_pass` as Pass 3 between const-fold and SIP.                                  |
| `test/rfl/lazy/chains.rfl` (new) | Behavioural assertions that chains form and execute correctly through the universal VM.     |
| `test/rfl/ops/idiom.rfl` (new)   | One assertion block per idiom row.                                                          |

---

## Phase 1 — Boundary materialisation (Layer B)

These tasks make it safe for producers to return lazy. After Phase 1 the codebase still produces no lazy values, so behaviour is unchanged — but the safety net is in place.

### Task 1: `ray_lazy_materialize` runs `ray_optimize`

**Files:**
- Modify: `src/ops/graph.c:1706-1719`

- [ ] **Step 1: Read current implementation**

  Read `src/ops/graph.c:1706-1719` and confirm the body matches the spec's quoted version (a `ray_execute` call sandwiched between unwrap and graph-free).

- [ ] **Step 2: Apply the change**

```c
ray_t* ray_lazy_materialize(ray_t* val) {
    if (!ray_is_lazy(val)) return val;

    ray_graph_t* g  = RAY_LAZY_GRAPH(val);
    ray_op_t*    op = RAY_LAZY_OP(val);

    /* Run the full optimizer pipeline before execution. ray_optimize
       may return a different root (e.g. predicate pushdown). */
    op = ray_optimize(g, op);

    ray_t* result = ray_execute(g, op);

    ray_graph_free(g);
    /* Clear graph pointer before releasing to prevent double-free in
     * ray_release_owned_refs */
    RAY_LAZY_GRAPH(val) = NULL;
    ray_release(val);
    return result;
}
```

- [ ] **Step 3: Build and run full test suite**

  Run: `make test`
  Expected: PASS. Today no producer returns lazy, so this code path is unreachable in practice — the change is dormant but in place. Sanitiser builds catch any regression in graph lifecycle.

- [ ] **Step 4: Commit**

```bash
git add src/ops/graph.c
git commit -m "fix(graph): ray_lazy_materialize must run ray_optimize before execute

The optimizer was running only inside query.c's select compiler. REPL-built
chains skipped it entirely. Wiring it into ray_lazy_materialize ensures every
materialisation sees the full optimization pipeline, regardless of how the
chain was built."
```

---

### Task 2: IPC send materialises lazy messages

**Files:**
- Modify: `src/core/ipc.c:1135` (`ray_ipc_send`), `:1199` (`ray_ipc_send_async`), `:1211` (`ray_ipc_send_verbose` — same path)

- [ ] **Step 1: Locate each send entry point and add the materialise prelude**

  At the top of each function, after the basic null/error guards but before any serialisation, add:

```c
if (ray_is_lazy(msg)) {
    msg = ray_lazy_materialize(msg);
    if (RAY_IS_ERR(msg)) return msg;        /* or ray_err code for _async */
}
```

  For `ray_ipc_send_async` (returns `ray_err_t`, not `ray_t*`): on materialise error, capture the err code from the err object, release it, return the err code. Pattern matches `ray_set_fn` at `eval.c:1112-1126`.

- [ ] **Step 2: Run full test suite**

  Run: `make test`
  Expected: PASS. No producer returns lazy yet, so the new branches are dormant but verified compileable.

- [ ] **Step 3: Commit**

```bash
git add src/core/ipc.c
git commit -m "fix(ipc): materialise lazy messages before serialising

ray_ipc_send and friends serialise raw bytes from msg. With the
universal-DAG-VM rollout msg may arrive as a lazy chain — materialise it
explicitly at the wire boundary."
```

---

### Task 3: Serde write materialises lazy objects

**Files:**
- Modify: `src/store/serde.c` (`ray_obj_save` — declared at `src/store/serde.h:78`)

- [ ] **Step 1: Add the materialise prelude to `ray_obj_save`**

  At the top of the function, before any serialisation:

```c
if (ray_is_lazy(obj)) {
    obj = ray_lazy_materialize(obj);
    if (RAY_IS_ERR(obj)) {
        ray_err_t code = ray_err_code_from_obj(obj);  /* or whatever pattern serde uses */
        ray_error_free(obj);
        return code;
    }
}
```

  Note from MEMORY.md: `ray_release` is a no-op on `RAY_IS_ERR` objects — use `ray_error_free`. Confirm the exact err-extraction helper by reading `serde.c`'s existing error paths first.

- [ ] **Step 2: Run full test suite**

  Run: `make test`
  Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git add src/store/serde.c
git commit -m "fix(serde): materialise lazy objects before persisting

ray_obj_save writes raw bytes; lazy chains must materialise at this
boundary so the on-disk image is a concrete value, not a deferred
computation."
```

---

### Task 4: Audit other write boundaries

**Files:**
- Read: `src/store/journal.c`, `src/store/serde.c`, `src/core/ipc.c`

- [ ] **Step 1: Confirm the boundary list is complete**

  Run: `grep -rn "ray_data\|->len" src/store/ src/core/ipc.c | grep -v test | head -30`

  For each call site that reads raw bytes from a `ray_t*` argument received from an external caller, verify it has a `ray_is_lazy` guard upstream OR is clearly running on already-materialised data (e.g. a column store's segment that came from disk).

- [ ] **Step 2: If any gaps found, add materialise prelude per the same pattern as Tasks 2–3**

  Otherwise, no commit — Phase 1 is complete.

---

## Phase 2 — Flip producers to return lazy (Layer A, partial)

Only the `AGG_VEC_VIA_DAG` macro flip in this phase. The single-op leaf cases in `ray_min_fn` / `ray_max_fn` (`agg.c:225, 254`) keep their `wrap+materialize` because they need `recast_i64_to_orig` post-processing that depends on a concrete result. That recast is a separate executor cleanup, deferred.

### Task 5: Flip `AGG_VEC_VIA_DAG` to return lazy

**Files:**
- Modify: `src/ops/agg.c:85-91`
- Test: `test/rfl/lazy/chains.rfl` (new file)

- [ ] **Step 1: Write the failing test**

  Create `test/rfl/lazy/chains.rfl`:

```rfl
;; Lazy chain — sum builds a single-op chain, materialises at REPL.
(set V [1 2 3 4 5])
(sum V) -- 15

;; sum + count over a stored vector — verifies the lazy infrastructure
;; doesn't perturb behaviour.
(count V) -- 5
(avg V) -- 3.0
```

  Per the test-harness memo, `.rfl` files under `test/rfl/lazy/` are auto-discovered.

- [ ] **Step 2: Run the test to verify the baseline passes**

  Run: `./rayforce.test -f chains`
  Expected: PASS (the assertions hold under the current eager implementation; this baseline guards against regression).

- [ ] **Step 3: Apply the macro change**

```c
#define AGG_VEC_VIA_DAG(x, ctor) do {                       \
    ray_graph_t* g = ray_graph_new(NULL);                   \
    if (!g) return ray_error("oom", NULL);                  \
    ray_op_t* in = ray_graph_input_vec(g, x);              \
    ray_op_t* op = ctor(g, in);                            \
    return ray_lazy_wrap(g, op);                            \
} while(0)
```

  (Drop the `ray_lazy_materialize` call.)

- [ ] **Step 4: Run the test again and verify it still passes**

  Run: `./rayforce.test -f chains`
  Expected: PASS. Now `sum`/`avg`/`count` return lazy; the REPL boundary materialises.

- [ ] **Step 5: Run the full test suite to confirm no regression**

  Run: `make test`
  Expected: PASS. Sanitisers must report clean.

- [ ] **Step 6: Commit**

```bash
git add src/ops/agg.c test/rfl/lazy/chains.rfl
git commit -m "feat(agg): AGG_VEC_VIA_DAG returns lazy instead of materialising

Aggregations over I64/F64 vectors now produce a lazy chain. The
materialisation happens at the universal boundary (REPL print, IPC send,
env bind). This is the first producer in the codebase that actually
returns lazy — exercising the if (ray_is_lazy(x)) extender branches in
agg.c that were dormant code until now."
```

---

## Phase 3 — Lift four ops into the DAG (Layer C)

Each task is one op and is fully self-contained: opcode + builder + executor + dump entry + lazy-append type rule + `*_fn` refactor. Land in any order.

### Task 6: Lift `distinct` into the DAG

**Files:**
- Modify: `src/ops/ops.h`, `src/ops/graph.c`, `src/ops/exec.c`, `src/ops/dump.c`, `src/ops/collection.c:700`
- Test: `test/rfl/lazy/chains.rfl`

- [ ] **Step 1: Write the failing test**

  Append to `test/rfl/lazy/chains.rfl`:

```rfl
;; distinct returns lazy — chain extends through count.
(set V [1 1 2 2 3 3])
(count (distinct V)) -- 3

;; The chain still works when reused.
(count (distinct [4 4 4 4])) -- 1
```

- [ ] **Step 2: Run the test to verify the second block currently passes (eager path) and the first block — well, also passes (eager path)**

  Run: `./rayforce.test -f chains`
  Expected: PASS (eager path produces the right answers today).

  This task converts the implementation, not the visible behaviour. The test guards against regression. The structural assertion (chain actually formed) is implicit — if the implementation is wrong the sanitisers + the ASan-instrumented graph lifecycle will surface it.

- [ ] **Step 3: Add the opcode**

  In `src/ops/ops.h`, after `OP_KNN_RERANK 103`:

```c
#define OP_DISTINCT  104  /* unique elements (preserves first occurrence)  */
```

- [ ] **Step 4: Add the DAG builder**

  In `src/ops/graph.c`, in the unary-builder cluster around `ray_count` (`graph.c:667`), add:

```c
ray_op_t* ray_distinct_op(ray_graph_t* g, ray_op_t* a) {
    return make_unary(g, OP_DISTINCT, a, a->out_type);
}
```

  And declare in `src/ops/ops.h` near the existing `ray_count_distinct`:

```c
ray_op_t* ray_distinct_op(ray_graph_t* g, ray_op_t* a);
```

- [ ] **Step 5: Add the lazy-append type rule**

  In `src/ops/graph.c:1683-1697` (`ray_lazy_append`'s out-type switch), add a case:

```c
case OP_DISTINCT:
    out_type = prev->out_type; break;     /* distinct preserves type */
```

- [ ] **Step 6: Add the executor case**

  In `src/ops/exec.c`'s opcode dispatch (the giant `switch` in `exec_node`, near the existing `case OP_COUNT_DISTINCT:` at line 1006), add:

```c
case OP_DISTINCT: {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;
    ray_t* result = ray_distinct_fn(input);   /* reuses the existing eager body */
    ray_release(input);
    return result;
}
```

  Note: this delegates to the existing `ray_distinct_fn`. After Step 8 that function will be the lazy-producer surface — but the executor invocation passes a concrete vector (the materialised input), so the function's `ray_is_lazy(x)` and `ray_is_atom(x)` guards short-circuit and it falls into the existing dedup logic. The function effectively splits roles: surface entry point (lazy producer) vs executor body (eager dedup). If/when we refactor for cleanliness, factor the dedup body into `exec_distinct_inner` and call that from both paths.

- [ ] **Step 7: Add the dump entry**

  In `src/ops/dump.c`, in `ray_opcode_name`, add:

```c
case OP_DISTINCT: return "DISTINCT";
```

- [ ] **Step 8: Refactor `ray_distinct_fn` to lazy producer**

  Replace `src/ops/collection.c:700` (`ray_distinct_fn`'s entry block) so the eager body is gated to "executor-callback" use only and the surface call returns lazy:

```c
ray_t* ray_distinct_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;

    /* Extend an existing chain. */
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_DISTINCT);

    /* Atoms (RAY_STR) and other non-vector forms still need eager
       handling — there's no DAG opcode for "distinct chars of a string"
       and no benefit to deferring. Fall through to the existing eager
       body in those cases. */
    if (!ray_is_vec(x)) {
        /* … existing string / list / table fallbacks unchanged … */
    }

    /* Concrete vector: start a fresh chain. */
    ray_graph_t* g  = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = ray_distinct_op(g, in);
    return ray_lazy_wrap(g, op);
}
```

  The existing dedup body (the hashset path at `collection.c:728-756`) becomes the executor's eager fallback — the executor calls back into `ray_distinct_fn` with a concrete vector, the function falls through to the eager body. To avoid an infinite loop on the executor path, factor the eager dedup body into a static helper `static ray_t* distinct_vec_eager(ray_t* x)` and have the executor case call that directly instead of round-tripping through `ray_distinct_fn`. Update Step 6's executor case accordingly:

```c
case OP_DISTINCT: {
    ray_t* input = exec_node(g, op->inputs[0]);
    if (!input || RAY_IS_ERR(input)) return input;
    ray_t* result = distinct_vec_eager(input);
    ray_release(input);
    return result;
}
```

- [ ] **Step 9: Run tests**

  Run: `./rayforce.test -f distinct -f chains`
  Expected: PASS. Run the existing distinct fixtures (`test/rfl/collection/distinct.rfl`, `test/rfl/null/distinct.rfl`) and the new chain fixture.

  Run: `make test`
  Expected: PASS. Sanitiser-clean.

- [ ] **Step 10: Commit**

```bash
git add src/ops/ops.h src/ops/graph.c src/ops/exec.c src/ops/dump.c src/ops/collection.c test/rfl/lazy/chains.rfl
git commit -m "feat(ops): lift distinct into the DAG

Adds OP_DISTINCT (opcode 104) with builder, executor, dump entry, and
lazy-append type rule. ray_distinct_fn now returns lazy when given a
concrete vector; the eager dedup body is factored into a static
helper that the executor calls directly.

(count (distinct V)) now builds a real chain SCAN(V) → OP_DISTINCT →
OP_COUNT that the optimizer sees end-to-end."
```

---

### Task 7: Lift `asc` into the DAG

**Files:**
- Modify: `src/ops/ops.h`, `src/ops/graph.c`, `src/ops/exec.c`, `src/ops/dump.c`, `src/ops/sort.c:3347`
- Test: `test/rfl/lazy/chains.rfl`

- [ ] **Step 1: Append fixture assertions**

```rfl
(count (asc V)) -- 5
(first (asc [3 1 2])) -- 1
```

- [ ] **Step 2: Add the opcode**

  In `src/ops/ops.h`:

```c
#define OP_ASC       105  /* sort ascending (preserves type)  */
```

- [ ] **Step 3: Add the DAG builder, lazy-append rule, executor case, dump entry**

  Same shape as Task 6 Steps 4–7 but for `OP_ASC`. The executor body factors out `asc_vec_eager(ray_t* x)` from the existing `ray_asc_fn` body at `sort.c:3347-3355` (the body is two lines — `uint8_t desc = 0; return ray_sort(&x, &desc, NULL, 1, n);`).

- [ ] **Step 4: Refactor `ray_asc_fn`**

```c
ray_t* ray_asc_fn(ray_t* x) {
    if (!x || RAY_IS_ERR(x)) return x;
    if (ray_is_lazy(x)) return ray_lazy_append(x, OP_ASC);
    if (ray_is_atom(x)) { ray_retain(x); return x; }
    if (!ray_is_vec(x)) return ray_error("type", "asc expects a vector");
    int64_t n = ray_len(x);
    if (n <= 1) { ray_retain(x); return x; }

    ray_graph_t* g  = ray_graph_new(NULL);
    if (!g) return ray_error("oom", NULL);
    ray_op_t* in = ray_graph_input_vec(g, x);
    ray_op_t* op = ray_asc_op(g, in);
    return ray_lazy_wrap(g, op);
}
```

- [ ] **Step 5: Run tests**

  Run: `./rayforce.test -f sort -f chains`
  Expected: PASS.

  Run: `make test`
  Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ops/ops.h src/ops/graph.c src/ops/exec.c src/ops/dump.c src/ops/sort.c test/rfl/lazy/chains.rfl
git commit -m "feat(ops): lift asc into the DAG

OP_ASC = 105. Same lazy-producer pattern as OP_DISTINCT."
```

---

### Task 8: Lift `desc` into the DAG

Same shape as Task 7. `OP_DESC = 106`. Refactors `ray_desc_fn` (`sort.c:3358`).

- [ ] All steps mirror Task 7. Commit message: "feat(ops): lift desc into the DAG".

---

### Task 9: Lift `reverse` into the DAG

Same shape. `OP_REVERSE = 107`. Refactors `ray_reverse_fn` (`collection.c:1710`).

- [ ] Read the existing `ray_reverse_fn` to confirm the eager body factors cleanly. If the body is more involved than asc/desc (likely it's a single-pass reverse), still wrap into `reverse_vec_eager` for symmetry.

- [ ] Same six-step pattern as Task 7. Commit message: "feat(ops): lift reverse into the DAG".

---

## Phase 4 — Idiom rewrite pass (Layer D)

### Task 10: Skeleton — `idiom.h` + `idiom.c` with empty table, wired into `ray_optimize`

**Files:**
- Create: `src/ops/idiom.h`, `src/ops/idiom.c`
- Modify: `src/ops/opt.c:2013-2064`

- [ ] **Step 1: Create the header**

  `src/ops/idiom.h`:

```c
/*
 *   Copyright (c) 2025-2026 Anton Kundenko <singaraiona@gmail.com>
 *   All rights reserved.
 *   [SPDX/MIT preamble — copy from src/ops/fuse.h]
 */

#ifndef RAY_IDIOM_H
#define RAY_IDIOM_H

#include "ops.h"

typedef bool      (*ray_idiom_pre_t)(ray_graph_t* g, ray_op_t* node);
typedef ray_op_t* (*ray_idiom_rw_t) (ray_graph_t* g, ray_op_t* node);

typedef struct {
    uint16_t          root_op;
    uint16_t          child0_op;
    ray_idiom_pre_t   pre;
    ray_idiom_rw_t    rewrite;
    const char*       name;
} ray_idiom_t;

extern const ray_idiom_t ray_idioms[];
extern const int         ray_idioms_count;

void ray_idiom_pass(ray_graph_t* g, ray_op_t* root);

#endif /* RAY_IDIOM_H */
```

- [ ] **Step 2: Create the body with dispatch index but no rows yet**

  `src/ops/idiom.c`:

```c
/* [SPDX/MIT preamble — copy from src/ops/fuse.c] */

#include "idiom.h"
#include "mem/sys.h"
#include <string.h>

#define RAY_IDIOM_OPCODE_CAP 128
#define RAY_IDIOM_MAX_ROWS    64

const ray_idiom_t ray_idioms[] = {
    /* rows added in tasks 11–16 */
};
const int ray_idioms_count = (int)(sizeof(ray_idioms) / sizeof(ray_idioms[0]));

_Static_assert(sizeof(ray_idioms) / sizeof(ray_idioms[0]) <= RAY_IDIOM_MAX_ROWS,
               "idiom row count exceeds dispatch index capacity");

static int8_t first_idiom[RAY_IDIOM_OPCODE_CAP];
static int8_t next_idiom [RAY_IDIOM_MAX_ROWS];
static bool   index_built;

static void build_index(void) {
    if (index_built) return;
    memset(first_idiom, -1, sizeof(first_idiom));
    memset(next_idiom,  -1, sizeof(next_idiom));
    for (int i = 0; i < ray_idioms_count; i++) {
        uint16_t op = ray_idioms[i].root_op;
        /* defensive — caught by the static assert in practice */
        if (op >= RAY_IDIOM_OPCODE_CAP) continue;
        next_idiom[i]   = first_idiom[op];
        first_idiom[op] = (int8_t)i;
    }
    index_built = true;
}

/* Find the ext node — duplicated from opt.c / fuse.c per the
   "self-contained file" convention. */
static ray_op_ext_t* find_ext(ray_graph_t* g, uint32_t node_id) {
    for (uint32_t i = 0; i < g->ext_count; i++) {
        if (g->ext_nodes[i] && g->ext_nodes[i]->base.id == node_id)
            return g->ext_nodes[i];
    }
    return NULL;
}
(void)find_ext;  /* unused until rewrites need it */

static bool is_ext_root(uint16_t opcode) {
    return opcode == OP_GROUP || opcode == OP_SORT || opcode == OP_JOIN ||
           opcode == OP_WINDOW || opcode == OP_WINDOW_JOIN || opcode == OP_SELECT;
}

static void try_rewrite(ray_graph_t* g, ray_op_t* node) {
    if (!node || (node->flags & OP_FLAG_DEAD)) return;
    if (is_ext_root(node->opcode)) return;
    if (node->opcode >= RAY_IDIOM_OPCODE_CAP) return;

    int idx = first_idiom[node->opcode];
    while (idx >= 0) {
        const ray_idiom_t* row = &ray_idioms[idx];
        if (node->inputs[0] && node->inputs[0]->opcode == row->child0_op) {
            if (!row->pre || row->pre(g, node)) {
                ray_op_t* repl = row->rewrite(g, node);
                if (repl) {
                    extern void redirect_consumers(ray_graph_t*, uint32_t, uint32_t);
                    /* redirect_consumers is static in opt.c — see Step 4 */
                    redirect_consumers(g, node->id, repl->id);
                    node->flags |= OP_FLAG_DEAD;
                    return;  /* first-match-wins */
                }
            }
        }
        idx = next_idiom[idx];
    }
}

void ray_idiom_pass(ray_graph_t* g, ray_op_t* root) {
    if (!g || !root || g->node_count == 0) return;
    build_index();

    /* Iterative post-order walk: children rewritten before parents so
       a parent match sees the latest shape of its children. Two-stack
       pattern — push roots onto stack1, drain into stack2 (reverse),
       pop stack2 to get post-order. */
    uint32_t nc = g->node_count;
    if (nc > UINT32_MAX / 4) return;  /* overflow guard, mirrors fuse.c */

    uint32_t cap = nc * 2;
    uint32_t stk1_local[256], stk2_local[256];
    uint32_t* stk1 = cap <= 256 ? stk1_local : (uint32_t*)ray_sys_alloc(cap * sizeof(uint32_t));
    uint32_t* stk2 = cap <= 256 ? stk2_local : (uint32_t*)ray_sys_alloc(cap * sizeof(uint32_t));
    if (!stk1 || !stk2) {
        if (stk1 && stk1 != stk1_local) ray_sys_free(stk1);
        if (stk2 && stk2 != stk2_local) ray_sys_free(stk2);
        return;
    }

    /* Visited-bit guard against re-entry on shared subgraphs.
       Allocated on the heap because ext_ids can also push onto the
       walk; mirror fuse.c:59-172's defensive sizing. */
    uint8_t visited_local[256];
    uint8_t* visited = nc <= 256 ? visited_local : (uint8_t*)ray_sys_alloc(nc);
    if (!visited) {
        if (stk1 != stk1_local) ray_sys_free(stk1);
        if (stk2 != stk2_local) ray_sys_free(stk2);
        return;
    }
    memset(visited, 0, nc);

    int sp1 = 0, sp2 = 0;
    stk1[sp1++] = root->id;
    while (sp1 > 0) {
        uint32_t nid = stk1[--sp1];
        if (nid >= nc || visited[nid]) continue;
        visited[nid] = 1;
        stk2[sp2++] = nid;

        ray_op_t* n = &g->nodes[nid];
        if (n->flags & OP_FLAG_DEAD) continue;
        for (int i = 0; i < n->arity && i < 2; i++) {
            if (n->inputs[i] && sp1 < (int)cap)
                stk1[sp1++] = n->inputs[i]->id;
        }
        /* Ext children for ext-bearing opcodes follow the exact same
           enumeration as fuse.c's count_refs (lines 111-167). For v1
           we skip ext roots in try_rewrite anyway (is_ext_root), so
           here we only traverse PAST them — children of an OP_GROUP's
           ext fields might themselves be rewriteable if exposed by a
           future row that does inspect ext data. For now this loop is
           a forward-looking placeholder; if you only handle main-DAG
           rewrites (the v1 catalog), you may omit it. */
    }

    /* Post-order: pop stk2 from top, call try_rewrite. Children were
       pushed onto stk2 first (because they were popped from stk1
       last in the parent's input loop), so popping stk2 yields
       parents-after-children, which is what we want. */
    while (sp2 > 0) {
        uint32_t nid = stk2[--sp2];
        try_rewrite(g, &g->nodes[nid]);
    }

    if (visited != visited_local) ray_sys_free(visited);
    if (stk1 != stk1_local) ray_sys_free(stk1);
    if (stk2 != stk2_local) ray_sys_free(stk2);
}
```

  **Note on `redirect_consumers`:** it's currently a static function in `opt.c:1312`. To use it from `idiom.c`, either (a) un-static it and add to `opt.h`/`ops.h`, or (b) duplicate the implementation. Choose (a) — it's a clean refactor and `redirect_consumers` is naturally a primitive. Move the function declaration to `opt.h` and drop `static` in `opt.c`. Same for `graph_alloc_node_opt` if rewrite functions need it (they will).

- [ ] **Step 3: Un-static `redirect_consumers` and `graph_alloc_node_opt`**

  In `src/ops/opt.c`: remove `static` from both function definitions.
  In `src/ops/opt.h`: add forward declarations.

- [ ] **Step 4: Wire the pass into `ray_optimize`**

  In `src/ops/opt.c:2013-2064`, after the `pass_constant_fold` block (`opt.c:2022-2024`) and before `sip_pass`:

```c
    /* Pass 3: Idiom rewrite */
    ray_profile_span_start("idiom");
    ray_idiom_pass(g, root);
    ray_profile_span_end("idiom");
    ray_profile_tick("idiom rewrite");
```

  Add `#include "idiom.h"` at the top of `opt.c` if not already present.

- [ ] **Step 5: Build and test**

  Run: `make test`
  Expected: PASS. The pass exists, runs on every graph, finds zero rows in its empty table, no-ops. Verifies the wiring and traversal don't crash on any of the existing test fixtures.

- [ ] **Step 6: Commit**

```bash
git add src/ops/idiom.h src/ops/idiom.c src/ops/opt.h src/ops/opt.c
git commit -m "feat(opt): scaffold idiom rewrite pass

Adds src/ops/idiom.{c,h} with the dispatch-index machinery and an empty
table. Wires pass_idiom_rewrite as Pass 3 in ray_optimize. Un-statics
redirect_consumers and graph_alloc_node_opt so idiom.c can use them.

No rewrites yet — this commit only verifies the pass runs cleanly
across the existing test suite."
```

---

### Task 11: Idiom row 1 — `(count (distinct v)) → OP_COUNT_DISTINCT(v)`

**Files:**
- Modify: `src/ops/idiom.c` (table)
- Test: `test/rfl/ops/idiom.rfl` (new)

- [ ] **Step 1: Write the failing test (assertion of behaviour, not structure)**

  Create `test/rfl/ops/idiom.rfl`:

```rfl
;; (count (distinct v)) — must equal hand-written reference
(set V [1 1 2 2 3 3 4 4 5])
(count (distinct V)) -- 5

;; with nulls
(count (distinct [1 0Nl 2 0Nl 1])) -- 2

;; empty
(count (distinct (as 'I64 ()))) -- 0

;; single element
(count (distinct [42])) -- 1

;; all equal
(count (distinct [7 7 7 7 7])) -- 1
```

- [ ] **Step 2: Run the test, verify baseline passes**

  Run: `./rayforce.test -f idiom`
  Expected: PASS. The behavioural answers are correct under both the eager path and the rewritten path; this test guards against semantic regression. Structural firing is not asserted (per the spec — too brittle).

- [ ] **Step 3: Add the rewrite function in `idiom.c`**

  Above the table:

```c
static ray_op_t* rw_count_distinct(ray_graph_t* g, ray_op_t* node) {
    /* node is OP_COUNT; node->inputs[0] is OP_DISTINCT.
       Replacement: OP_COUNT_DISTINCT(distinct->inputs[0]). */
    ray_op_t* distinct = node->inputs[0];
    if (!distinct || !distinct->inputs[0]) return NULL;
    ray_op_t* src = distinct->inputs[0];

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_COUNT_DISTINCT;
    repl->arity     = 1;
    repl->inputs[0] = src;
    repl->out_type  = RAY_I64;
    repl->est_rows  = 1;
    return repl;
}
```

- [ ] **Step 4: Add the row to the table**

```c
const ray_idiom_t ray_idioms[] = {
    { OP_COUNT, OP_DISTINCT, NULL, rw_count_distinct,
      "count(distinct) -> count_distinct" },
};
```

- [ ] **Step 5: Run the test**

  Run: `./rayforce.test -f idiom`
  Expected: PASS. Behaviour unchanged; the rewrite kicks in but produces semantically identical results. Validate via profiling or printf-debug that the rewrite actually fires (optional sanity check; leave the printf out of the commit).

  Run: `make test`
  Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ops/idiom.c test/rfl/ops/idiom.rfl
git commit -m "feat(opt): idiom rewrite — count(distinct v) -> count_distinct(v)

First row in the idiom table. Validates the matcher + rewrite + redirect
machinery end to end on the headline pattern."
```

---

### Task 12: Idiom rows 2–4 — `(count (asc|desc|reverse v)) → (count v)`

**Files:**
- Modify: `src/ops/idiom.c` (one shared rewrite fn + 3 rows)
- Modify: `test/rfl/ops/idiom.rfl`

- [ ] **Step 1: Append fixture**

```rfl
;; cardinality-preserving rewrites
(count (asc     [3 1 4 1 5 9 2 6])) -- 8
(count (desc    [3 1 4 1 5 9 2 6])) -- 8
(count (reverse [3 1 4 1 5 9 2 6])) -- 8

;; with nulls — asc/desc/reverse preserve nulls; count includes them
(count (asc [1 0Nl 2])) -- 3
```

- [ ] **Step 2: Add a single shared rewrite function**

  In `idiom.c`:

```c
static ray_op_t* rw_count_passthrough(ray_graph_t* g, ray_op_t* node) {
    /* (count (X v)) -> (count v) for any X that preserves cardinality.
       Replacement: a fresh OP_COUNT directly over X's input. */
    ray_op_t* inner = node->inputs[0];
    if (!inner || !inner->inputs[0]) return NULL;
    ray_op_t* src = inner->inputs[0];

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_COUNT;
    repl->arity     = 1;
    repl->inputs[0] = src;
    repl->out_type  = RAY_I64;
    repl->est_rows  = 1;
    return repl;
}
```

- [ ] **Step 3: Add the three rows**

```c
const ray_idiom_t ray_idioms[] = {
    { OP_COUNT, OP_DISTINCT, NULL, rw_count_distinct,    "count(distinct) -> count_distinct" },
    { OP_COUNT, OP_ASC,      NULL, rw_count_passthrough, "count(asc) -> count" },
    { OP_COUNT, OP_DESC,     NULL, rw_count_passthrough, "count(desc) -> count" },
    { OP_COUNT, OP_REVERSE,  NULL, rw_count_passthrough, "count(reverse) -> count" },
};
```

- [ ] **Step 4: Test**

  Run: `./rayforce.test -f idiom`
  Expected: PASS.

  Run: `make test`
  Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add src/ops/idiom.c test/rfl/ops/idiom.rfl
git commit -m "feat(opt): idiom rewrites — count over asc/desc/reverse

Three cardinality-preserving rewrites that drop the inner sort/reverse
since count only cares about length. Shared rw_count_passthrough handles
all three."
```

---

### Task 13: Idiom rows 5–6 — `(first|last (asc v)) → OP_MIN|OP_MAX(v)` with null gate

**Files:**
- Modify: `src/ops/idiom.c`
- Modify: `test/rfl/ops/idiom.rfl`

- [ ] **Step 1: Append fixture**

```rfl
;; (first (asc v)) on null-free input — should equal (min v)
(first (asc [3 1 4 1 5 9 2 6])) -- 1
(last  (asc [3 1 4 1 5 9 2 6])) -- 9

;; (first (asc v)) on null-bearing input — must NOT rewrite to min;
;; first(asc) returns null (asc puts nulls first); min skips nulls.
(nil? (first (asc [1 0Nl 2]))) -- true
;; min would be 1 — verify the slow path still runs.

;; (last (asc v)) on null-bearing input — last after asc is the
;; largest non-null in this case (nulls at the start).
(last (asc [1 0Nl 2])) -- 2
```

- [ ] **Step 2: Add the precondition function**

  In `idiom.c`:

```c
/* True only when the input vector to (asc …) is statically known to
   have no nulls. Walks one node — the input to the asc — and reads
   its out_attrs (if tracked) or out_type+constness. Returns false on
   uncertainty (safe default — slow path runs). */
static bool pre_no_nulls_on_asc_input(ray_graph_t* g, ray_op_t* node) {
    (void)g;
    /* node = OP_FIRST (or OP_LAST); node->inputs[0] = OP_ASC;
       inspect node->inputs[0]->inputs[0] (the source vector). */
    ray_op_t* asc = node->inputs[0];
    if (!asc || !asc->inputs[0]) return false;
    ray_op_t* src = asc->inputs[0];

    /* OP_CONST_VEC and OP_SCAN are the cases where we can read a
       known null-state. For other opcodes (computed inputs), bail —
       false negative is fine. */
    if (src->opcode == OP_CONST_VEC) {
        /* The literal is stashed in the ext data — see ray_const_vec
           and how opt.c reads literals at e.g. opt.c:1880-1890. */
        ray_op_ext_t* ext = find_ext(g, src->id);
        if (!ext || !ext->literal) return false;
        ray_t* lit = ext->literal;
        return !(lit->attrs & RAY_ATTR_HAS_NULLS);
    }
    if (src->opcode == OP_SCAN) {
        /* OP_SCAN's source column header carries RAY_ATTR_HAS_NULLS.
           ext data for OP_SCAN holds the column reference — pattern at
           query.c's compile_expr_dag's ray_scan branch. */
        /* … verify column-header attrs read pattern by reading the
           OP_SCAN executor in src/ops/exec.c first … */
        return false;  /* conservative until that read is verified */
    }
    return false;
}
```

  **Caveat:** the OP_SCAN branch is conservative until the implementer reads the OP_SCAN executor and confirms the pattern for reading column-header `attrs`. For the first cut, returning `false` for OP_SCAN means the rewrite only fires on OP_CONST_VEC inputs (literal vectors written inline, e.g. `[3 1 4 1 5 9 2 6]`). The test fixture above exercises exactly that case. Extending to OP_SCAN is a follow-up that requires reading `exec_scan` and identifying where the column header's attrs are accessible from the optimizer.

- [ ] **Step 3: Add rewrite functions**

```c
static ray_op_t* rw_first_asc_to_min(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* asc = node->inputs[0];
    if (!asc || !asc->inputs[0]) return NULL;
    ray_op_t* src = asc->inputs[0];

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_MIN;
    repl->arity     = 1;
    repl->inputs[0] = src;
    repl->out_type  = src->out_type;
    repl->est_rows  = 1;
    return repl;
}

static ray_op_t* rw_last_asc_to_max(ray_graph_t* g, ray_op_t* node) {
    ray_op_t* asc = node->inputs[0];
    if (!asc || !asc->inputs[0]) return NULL;
    ray_op_t* src = asc->inputs[0];

    ray_op_t* repl = graph_alloc_node_opt(g);
    if (!repl) return NULL;
    repl->opcode    = OP_MAX;
    repl->arity     = 1;
    repl->inputs[0] = src;
    repl->out_type  = src->out_type;
    repl->est_rows  = 1;
    return repl;
}
```

- [ ] **Step 4: Add the rows**

```c
const ray_idiom_t ray_idioms[] = {
    { OP_COUNT, OP_DISTINCT, NULL,                       rw_count_distinct,    "count(distinct) -> count_distinct" },
    { OP_COUNT, OP_ASC,      NULL,                       rw_count_passthrough, "count(asc) -> count" },
    { OP_COUNT, OP_DESC,     NULL,                       rw_count_passthrough, "count(desc) -> count" },
    { OP_COUNT, OP_REVERSE,  NULL,                       rw_count_passthrough, "count(reverse) -> count" },
    { OP_FIRST, OP_ASC,      pre_no_nulls_on_asc_input,  rw_first_asc_to_min,  "first(asc) -> min  [no-nulls]" },
    { OP_LAST,  OP_ASC,      pre_no_nulls_on_asc_input,  rw_last_asc_to_max,   "last(asc) -> max   [no-nulls]" },
};
```

- [ ] **Step 5: Test**

  Run: `./rayforce.test -f idiom`
  Expected: PASS. The null-bearing inputs hit the precondition's `false` return and the slow path produces the right answers; the no-null literal inputs trigger the rewrite and produce the same answers.

  Run: `make test`
  Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add src/ops/idiom.c test/rfl/ops/idiom.rfl
git commit -m "feat(opt): idiom rewrites — first/last over asc, gated by nulls

Adds the algebraic rewrites first(asc v) -> min(v) and last(asc v) ->
max(v), gated by a precondition that requires the input vector to be
statically known null-free. OP_CONST_VEC inputs read the literal's
attrs; OP_SCAN remains conservative pending a follow-up that reads
the column-header attrs from the optimizer."
```

---

## Spec coverage check

| Spec item                                              | Plan task                |
|--------------------------------------------------------|--------------------------|
| Layer A: producers return lazy (AGG_VEC_VIA_DAG)       | Task 5                   |
| Layer A: existing aggs already chain-extend            | (no change — already done) |
| Layer B: REPL/format/eval/cmp/builtins/collection      | (no change — already in place per audit) |
| Layer B: IPC send                                      | Task 2                   |
| Layer B: serde write                                   | Task 3                   |
| Layer B: journal                                       | Task 4 (audit confirms; journal sees post-serialised bytes via IPC path) |
| Layer B: ray_lazy_materialize runs ray_optimize         | Task 1                   |
| Layer C: lift distinct                                 | Task 6                   |
| Layer C: lift asc                                      | Task 7                   |
| Layer C: lift desc                                     | Task 8                   |
| Layer C: lift reverse                                  | Task 9                   |
| Layer D: pass + dispatch index + insertion in optimize | Task 10                  |
| Layer D: row 1 (count distinct)                        | Task 11                  |
| Layer D: rows 2-4 (count over asc/desc/reverse)        | Task 12                  |
| Layer D: rows 5-6 (first/last over asc, null-gated)    | Task 13                  |
| Producer audit                                         | (mentioned in Task 4; full audit deferred to follow-up per spec) |
| Profiling / observability                              | Task 10 (span+tick wired) |
| `name` field reserved for debug-opt                    | Task 10 (struct includes; no consumer yet) |

## Open follow-ups (out of scope for this plan, captured for the next round)

- `ray_min_fn` / `ray_max_fn` leaf-case still materialises eagerly because of `recast_i64_to_orig`. Cleanup: teach the executor to honour `OP_MIN.out_type` instead of unconditionally returning I64.
- `OP_SCAN` precondition path for null-state — needs read of `exec_scan` to identify the column header's accessible attrs.
- The wider producer audit (lifting other `*_fn`s into the DAG when there's payoff).
- New executor opcodes for richer rewrites: `OP_COUNT_WHERE` (count over filter), algebraic `(sum (* c v)) → c * sum(v)`.
- Surface-language sugar: `count_distinct` as a registered Rayfall name.
- Grouped-form rewrites inside `OP_GROUP`'s `agg_ins[]`.
