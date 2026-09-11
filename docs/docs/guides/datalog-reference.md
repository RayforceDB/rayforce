# Datalog: Rules & Queries

## What is Datalog?

Datalog is a **rule-based declarative query language**. You define facts and rules, then ask questions. The engine figures out all possible answers automatically by applying rules until no new facts can be derived.

Unlike SQL, Datalog handles **recursive queries** naturally — transitive closure, reachability, and graph traversal are first-class operations, not awkward CTEs bolted on as an afterthought.

!!! note "Key idea"

    You declare logical relationships. Rayforce compiles them into the same vectorized, morsel-parallel execution pipeline used by the rest of the engine. No interpretation overhead — rules become DAG nodes.

## EAV Triple Storage

Datalog in Rayforce uses **Entity-Attribute-Value (EAV)** triples as its storage model. Every fact is a triple `(entity, attribute, value)` stored in a columnar datoms table.

### Creating a datoms database

Use `(datoms)` to create an empty EAV database, then `(assert-fact db entity attribute value)` to add triples:

```lisp
;; Create an empty EAV database
(set db (datoms))

;; Assert facts: entity 1 has name, dept, salary
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 1 'dept 'Engineering))
(set db (assert-fact db 1 'salary 80000))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 2 'dept 'Sales))
(set db (assert-fact db 2 'salary 60000))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 3 'dept 'Engineering))
(set db (assert-fact db 3 'salary 90000))
```

Each call to `assert-fact` returns a new database with the triple added. The underlying storage is a three-column table `[e, a, v]` backed by Rayforce's columnar vectors.

### Scanning by attribute

Use `(scan-eav db attribute)` to query all entities with a given attribute, or `(scan-eav db entity attribute)` to get a specific value:

```lisp
;; All salaries: returns a table of [entity, value]
(show (scan-eav db 'salary))

;; Specific entity's salary: returns the scalar value
(println (scan-eav db 3 'salary))  ;; => 90000
```

**Verified output** (from `scan-eav db 'salary`):

```text
All salaries:
┌─────┬───────────────────────────────┐
│  e  │               v               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 80000                         │
│ 2   │ 60000                         │
│ 3   │ 90000                         │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
Entity 3 salary:
90000
```

## Rules

Rules define derived relations. A rule has a **head** (the relation being defined) and a **body** (the conditions that must hold):

```text
(rule (head ?vars...)
  ;; body clauses — all must be satisfied
  (?e :attr ?v)
  (?e :other-attr ?w))
```

Variables start with `?`. When the same variable appears in multiple clauses, it acts as a **join condition** — the values must be equal.

### Simple rule example

```lisp
;; Define "employee" as entities with both name and dept
(rule (employee ?e ?n ?d)
  (?e :name ?n)
  (?e :dept ?d))

;; Query using the rule
(show (query db (find ?n ?d) (where (employee ?e ?n ?d))))
```

This rule says: "an employee is any entity `?e` that has both a `:name` attribute (bound to `?n`) and a `:dept` attribute (bound to `?d`)." The shared variable `?e` across the two clauses produces a join on entity ID.

**Verified output**:

```text
Employees (via rule):
┌─────┬───────────────────────────────┐
│ ?n  │              ?d               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 340 │ 342                           │
│ 344 │ 345                           │
│ 346 │ 342                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

### OR semantics with multiple clauses

Define multiple rules with the **same head** to express disjunction (OR). Each rule clause contributes its own set of results, and they are combined:

```lisp
;; Two ways to be "reachable"
(rule (reachable ?x ?y) (?x :edge ?y))              ;; base: direct edge
(rule (reachable ?x ?z) (?x :edge ?y) (reachable ?y ?z))  ;; recursive
```

## Queries

Queries retrieve data from the datoms database using pattern matching. The syntax is:

```text
(query db (find ?vars...) (where clauses...))
```

The `find` clause specifies which variables to return. The `where` clause contains triple patterns and rule invocations that constrain the results.

### Simple pattern query

```lisp
;; Find all entities and their names
(show (query db (find ?e ?n) (where (?e :name ?n))))
```

**Verified output**:

```text
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 2   │ 344                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

The `?n` column contains symbol intern IDs. Use `(sym-name id)` to convert them to readable names (see [sym-name](#sym-name) below).

### Join query

When multiple patterns share a variable, Rayforce compiles them into a join:

```lisp
;; Find name + department (join on entity ?e)
(show (query db (find ?n ?d) (where (?e :name ?n) (?e :dept ?d))))
```

**Verified output**:

```text
┌─────┬───────────────────────────────┐
│ ?n  │              ?d               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 340 │ 342                           │
│ 344 │ 345                           │
│ 346 │ 342                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

### Constant value patterns

Use a literal value instead of a variable to filter by a specific attribute value:

```lisp
;; Only entities in the Engineering department
(show (query db (find ?e ?n) (where (?e :name ?n) (?e :dept 'Engineering))))
```

**Verified output** (filters to entities 1 and 3):

```text
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

### Wildcard patterns

Use `_` to match any value without binding it to a variable:

```lisp
;; All entities that have any dept attribute
(show (query db (find ?e) (where (?e :dept _))))
```

**Verified output**:

```text
┌─────────────────────────────────────┐
│                 ?e                  │
│                 I64                 │
├─────────────────────────────────────┤
│ 1                                   │
│ 2                                   │
│ 3                                   │
├─────────────────────────────────────┤
│ 3 rows (3 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
```

### How queries compile to the DAG

Under the hood each rule is evaluated by materialisation: every positive body
atom is filtered on its constants and repeated variables, joined with the
accumulated result on shared variables (hash join, morsel-parallel), then
negations, comparisons, assignments and aggregates are applied in declared
order, and the head projection is de-duplicated and merged into the derived
relation. Recursive rules run to fixpoint with semi-naive evaluation. Joins
execute on the same operators as `select`, but a whole rule is not planned as
one DAG: body atoms are joined in the order written, and comparisons are
applied after the joins that bind their variables.

## Negation

Use `(not (pattern))` in a `where` clause to exclude entities matching a pattern. Negation compiles to `OP_ANTIJOIN` — keeping rows from the positive clauses that have no match in the negated pattern.

```lisp
;; Mark some employees as managers
(set db (assert-fact db 1 'manager 'true))
(set db (assert-fact db 3 'manager 'true))

;; Find employees who are NOT managers
(show (query db (find ?e ?n)
  (where (?e :name ?n)
         (not (?e :manager ?m)))))
```

**Verified output** (only Bob, entity 2, is not a manager):

```text
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 2   │ 344                           │
├─────┴───────────────────────────────┤
│ 1 rows (1 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

The negated pattern must share at least one variable with a positive clause (here `?e`) so the antijoin knows which column to match on.

## Recursive Rules & Fixpoint

The real power of Datalog is **recursive rules**. Define a rule that references itself, and the engine automatically computes the transitive closure by iterating until no new facts are produced (the "fixpoint").

### Transitive closure example

```lisp
;; Build a directed graph: 1->2->3->4
(set gdb (datoms))
(set gdb (assert-fact gdb 1 'edge 2))
(set gdb (assert-fact gdb 2 'edge 3))
(set gdb (assert-fact gdb 3 'edge 4))

;; Base case: direct edge
(rule (reachable ?x ?y) (?x :edge ?y))

;; Recursive case: edge + reachability
(rule (reachable ?x ?z) (?x :edge ?y) (reachable ?y ?z))

;; Query: all reachable pairs
(show (query gdb (find ?x ?y) (where (reachable ?x ?y))))
```

**Verified output** (6 reachable pairs from 3 edges: direct + transitive):

```text
┌─────┬───────────────────────────────┐
│ ?x  │              ?y               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 2                             │
│ 2   │ 3                             │
│ 3   │ 4                             │
│ 1   │ 3                             │
│ 2   │ 4                             │
│ 1   │ 4                             │
├─────┴───────────────────────────────┤
│ 6 rows (6 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

### Semi-naive evaluation

Rayforce uses **semi-naive evaluation** for fixpoint computation, which is significantly faster than naive re-evaluation:

1. **Base pass:** Apply all non-recursive rule clauses to produce initial facts
2. **Delta iteration:** In each round, only use *newly derived* facts (the "delta") as input to rule bodies
3. **Antijoin:** Remove facts already known from the delta to keep only truly new derivations
4. **Terminate:** When the delta is empty, the fixpoint has been reached

Each iteration compiles to a fresh DAG and calls `ray_execute`. The antijoin step uses `ray_antijoin` to efficiently filter out previously known facts.

## Stratification

When rules use negation, evaluation order matters. A negated predicate must be fully computed before it can be used in an antijoin. Rayforce handles this automatically.

The engine builds a **dependency graph** among predicates, detects which ones depend on negated versions of others, and partitions them into **strata** (layers). Each stratum is evaluated to fixpoint before the next begins.

- Strata are computed via topological sort on the dependency graph
- If a negation cycle is detected (predicate A negates B, and B negates A), the engine rejects the program with an "unstratifiable" error
- No user action is needed — stratification is handled internally during `query` evaluation

## Pull Queries

Pull queries provide **entity-centric retrieval** — given an entity ID, return all (or selected) attributes as a dictionary:

```lisp
;; Pull all attributes for entity 1
(println (pull db 1))

;; Pull only specific attributes
(println (pull db 2 [name salary]))
```

**Verified output**:

```text
Pull all attributes for entity 1:
{name:340 dept:342 salary:80000 manager:354}
Pull name + salary for entity 2:
{name:344 salary:60000}
```

Pull returns a dict keyed by attribute symbol. Symbol attribute values appear as intern IDs — use `(sym-name id)` to convert them to readable names. Non-symbol values (like `salary`) are stored verbatim.

## sym-name

EAV queries and pull results store symbol values as integer intern IDs for efficient joining and comparison. Use `(sym-name id)` to convert an intern ID back to a readable symbol atom.

```lisp
;; Get the intern ID from a pull result
(set p (pull db 1))
(set name-id (get p 'name))  ;; index the dict by attribute key, not position
(println name-id)            ;; => 340 (intern ID)
(println (sym-name name-id)) ;; => 'Alice
```

**Verified output**:

```text
340
'Alice
```

`sym-name` accepts both scalar i64 values and i64 vectors. On a vector, it returns a SYM vector with each ID resolved. This is useful for converting entire columns of intern IDs to readable names.

## Programmatic API

For advanced use cases, Rayforce provides a programmatic Datalog API that gives fine-grained control over the evaluation process:

| Function | Description |
|---|---|
| `(dl-program)` | Create a new empty Datalog program |
| `(dl-add-edb prog 'name table arity)` | Register an extensional database (base facts) table |
| `(dl-stratify prog)` | Compute evaluation strata for the program |
| `(dl-eval prog)` | Evaluate the program to fixpoint |
| `(dl-query prog 'pred)` | Retrieve the result table for a predicate |
| `(dl-provenance prog 'pred)` | Reserved provenance hook; currently returns `domain: not available` |

### Example

```lisp
;; Create a program and register base facts
(set prog (dl-program))
(set edges (table ['from 'to] (list [1 2 3] [2 3 4])))
(dl-add-edb prog 'edge edges 2)

;; Stratify and evaluate
(dl-stratify prog)
(dl-eval prog)

;; Query results
(show (dl-query prog 'edge))
```

**Verified output**:

```text
┌──────────┬──────────────────────────┐
│ edge__c0 │         edge__c1         │
│   I64    │           I64            │
├──────────┼──────────────────────────┤
│ 1        │ 2                        │
│ 2        │ 3                        │
│ 3        │ 4                        │
├──────────┴──────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

## retract-fact

Use `(retract-fact db entity attr value)` to remove a triple from the datoms database:

```lisp
;; Remove Bob's name fact
(set db (retract-fact db 2 'name 'Bob))

;; Verify: only Alice and Charlie remain
(show (query db (find ?e ?n) (where (?e :name ?n))))
```

**Verified output** (2 rows after retraction):

```text
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
```

## How Datalog Maps to Rayforce

Every Datalog concept compiles down to existing Rayforce DAG operations. The Datalog layer is purely a compilation frontend — the engine does all the heavy lifting.

| Datalog Concept | Rayforce Operation | Description |
|---|---|---|
| Triple pattern `(?e :attr ?v)` | `ray_scan` + `ray_filter` | Indexed column scan filtered by attribute |
| Shared variable (join) | `ray_join` | Hash join on shared variable column |
| Constant value `(?e :dept 'Engineering)` | `ray_filter` | Equality filter on the value column |
| Wildcard `(?e :dept _)` | `ray_scan` | Attribute scan without value constraint |
| Negation `(not ...)` | `ray_antijoin` | Keep rows not present in another result |
| OR rules (same head) | `union-all` + `distinct` | Combine results and deduplicate |
| Fixpoint (recursion) | Loop with `ray_execute` | Iterate until delta is empty |
| Stratification | Topological sort | Compute evaluation order from dependency graph |
| `(find ?a ?n)` | Projection | Select output columns from the result |
| `(pull db entity)` | `ray_scan` on EAV index | Entity attribute scan and collection |
| Rule invocation | Subgraph expansion | Inline the rule's compiled DAG as a subplan |

!!! note "Performance"

    Joins and distinct inside a rule use the parallel `select` operators. Rule-level
    join ordering, predicate pushdown across atoms and plan reuse between queries are
    not performed; write the most selective atom first.

## Constants in patterns

In a `where` clause or rule body, a quoted symbol (`'Alice`) or keyword
(`:name`) is a literal; a bare unquoted symbol (`sid`) is evaluated as a
variable reference to a bound value (integer, float, symbol or string). Head
arguments still treat a bare symbol as a literal constant.

## Arithmetic

Integer expressions in `(= ?y (+ ?x 1))` and comparisons are 64-bit and checked:
overflow, division by zero, `INT64_MIN / -1` and any null operand produce the
null integer `0Nl`. Float expressions follow IEEE 754 (division by zero gives
`0Nf`). The error code surfaced for a failed query evaluation is `domain`.

## Limits

| Limit | Value | On overflow |
|---|---|---|
| Variables in `find` | 16 | error `find supports at most 16 variables` |
| Arity of a relation | 16 | error at rule or EDB registration |
| Body literals per rule | 16 | error `too many body literals` |
| Rules per program | 128 | error `too many rules` |
| Relations per program | 64 | error |
| Strata | 16 | error |
| Group keys in an aggregate | 8 | error |

An unknown relation name in a body is an error (`unknown relation 'name'`),
not an empty result.

## Symbol storage

`assert-fact` stores symbol values in the `v` column with a type tag, so the
symbol `'Alice` and the integer equal to its intern id are different values.
Query results, `pull` and `scan-eav` return the plain intern id; use `sym-name`
to read it back. Inspecting the raw datoms table shows the tagged encoding.

A datoms table whose `v` column is a real symbol vector (hand-built or
loaded) is also recognised as holding symbols. A legacy table whose `v`
column holds bare integer intern ids is not — those integers stay integers
and are not matched against a symbol or keyword literal. Positive integer
values of 2^61 and above cannot be stored in the `v` column without being
masked on the way out; this is a limit of the tag encoding, and it applies
only to values that live in (or are derived from) the datoms `v` column.
Every other column — an ordinary relation's column, or a value computed by
Datalog arithmetic — is returned verbatim, whatever its magnitude.

A derived relation fed both a symbol projected from `v` and an equal bare
integer from another source matches both on a symbol literal: outside the
`eav` relation the comparison is deliberately lenient about the tag, so the
two values are indistinguishable once they share a column.

## Complete Example

The following example demonstrates the full Datalog workflow: EAV storage, queries with constants and wildcards, rules, negation, transitive closure, pull queries, sym-name, and retract-fact. This is the content of `examples/rfl/datalog.rfl`.

### Source

```lisp
; Datalog Example
; Demonstrates: datoms, assert-fact, retract-fact, rules, queries,
; negation, fixpoint (transitive closure), pull, sym-name,
; constant values, and wildcard patterns

; -- 1. EAV storage --
(set db (datoms))
(set db (assert-fact db 1 'name 'Alice))
(set db (assert-fact db 1 'dept 'Engineering))
(set db (assert-fact db 1 'salary 80000))
(set db (assert-fact db 2 'name 'Bob))
(set db (assert-fact db 2 'dept 'Sales))
(set db (assert-fact db 2 'salary 60000))
(set db (assert-fact db 3 'name 'Charlie))
(set db (assert-fact db 3 'dept 'Engineering))
(set db (assert-fact db 3 'salary 90000))

; -- 2. Simple query --
(println "--- All names ---")
(set r (query db (find ?e ?n) (where (?e :name ?n))))
(show r)

; -- 3. Join query --
(println "--- Name + Department ---")
(set r (query db (find ?n ?d) (where (?e :name ?n) (?e :dept ?d))))
(show r)

; -- 4. Constant value pattern --
(println "--- Engineers only (constant pattern) ---")
(set r (query db (find ?e ?n) (where (?e :name ?n) (?e :dept 'Engineering))))
(show r)

; -- 5. Wildcard pattern --
(println "--- Entities with any dept (wildcard _) ---")
(set r (query db (find ?e) (where (?e :dept _))))
(show r)

; -- 6. Rule: define "employee" relation --
(rule (employee ?e ?n ?d)
  (?e :name ?n)
  (?e :dept ?d))

(println "--- Employees (via rule) ---")
(set r (query db (find ?n ?d) (where (employee ?e ?n ?d))))
(show r)

; -- 7. Negation --
(set db (assert-fact db 1 'manager 'true))
(set db (assert-fact db 3 'manager 'true))

(println "--- Non-managers (negation) ---")
(set r (query db (find ?e ?n) (where (?e :name ?n) (not (?e :manager ?m)))))
(show r)

; -- 8. Transitive closure (recursive rules + fixpoint) --
(set gdb (datoms))
(set gdb (assert-fact gdb 1 'edge 2))
(set gdb (assert-fact gdb 2 'edge 3))
(set gdb (assert-fact gdb 3 'edge 4))

(rule (reachable ?x ?y) (?x :edge ?y))
(rule (reachable ?x ?z) (?x :edge ?y) (reachable ?y ?z))

(println "--- Reachable pairs (transitive closure) ---")
(set r (query gdb (find ?x ?y) (where (reachable ?x ?y))))
(show r)

; -- 9. Pull queries --
(println "--- Pull entity 1 (all attributes) ---")
(println (pull db 1))

(println "--- Pull entity 2 (name + salary only) ---")
(println (pull db 2 [name salary]))

; -- 10. sym-name: readable output --
(println "--- sym-name: convert intern IDs ---")
(set p (pull db 1))
(set name-id (get p 'name))
(println name-id)
(println (sym-name name-id))

; -- 11. retract-fact --
(println "--- Before retract ---")
(set r (query db (find ?e ?n) (where (?e :name ?n))))
(show r)

(set db (retract-fact db 2 'name 'Bob))
(println "--- After retract (Bob removed) ---")
(set r (query db (find ?e ?n) (where (?e :name ?n))))
(show r)

; -- 12. Scan-eav: low-level attribute lookup --
(println "--- All salaries (scan-eav) ---")
(show (scan-eav db 'salary))

(println "--- Entity 3 salary ---")
(println (scan-eav db 3 'salary))
```

### Output

Running `./rayforce examples/rfl/datalog.rfl` produces:

```text
--- All names ---
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 2   │ 344                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Name + Department ---
┌─────┬───────────────────────────────┐
│ ?n  │              ?d               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 340 │ 342                           │
│ 344 │ 345                           │
│ 346 │ 342                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Engineers only (constant pattern) ---
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Entities with any dept (wildcard _) ---
┌─────────────────────────────────────┐
│                 ?e                  │
│                 I64                 │
├─────────────────────────────────────┤
│ 1                                   │
│ 2                                   │
│ 3                                   │
├─────────────────────────────────────┤
│ 3 rows (3 shown) 1 columns (1 shown)│
└─────────────────────────────────────┘
--- Employees (via rule) ---
┌─────┬───────────────────────────────┐
│ ?n  │              ?d               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 340 │ 342                           │
│ 344 │ 345                           │
│ 346 │ 342                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Non-managers (negation) ---
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 2   │ 344                           │
├─────┴───────────────────────────────┤
│ 1 rows (1 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Reachable pairs (transitive closure) ---
┌─────┬───────────────────────────────┐
│ ?x  │              ?y               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 2                             │
│ 2   │ 3                             │
│ 3   │ 4                             │
│ 1   │ 3                             │
│ 2   │ 4                             │
│ 1   │ 4                             │
├─────┴───────────────────────────────┤
│ 6 rows (6 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Pull entity 1 (all attributes) ---
{name:340 dept:342 salary:80000 manager:354}
--- Pull entity 2 (name + salary only) ---
{name:344 salary:60000}
--- sym-name: convert intern IDs ---
340
'Alice
--- Before retract ---
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 2   │ 344                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- After retract (Bob removed) ---
┌─────┬───────────────────────────────┐
│ ?e  │              ?n               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 340                           │
│ 3   │ 346                           │
├─────┴───────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- All salaries (scan-eav) ---
┌─────┬───────────────────────────────┐
│  e  │               v               │
│ I64 │              I64              │
├─────┼───────────────────────────────┤
│ 1   │ 80000                         │
│ 2   │ 60000                         │
│ 3   │ 90000                         │
├─────┴───────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown)│
└─────────────────────────────────────┘
--- Entity 3 salary ---
90000
```

!!! note "Note"

    Symbol intern IDs (like 340 for `'Alice` in the sample above) are assigned at runtime and may differ between sessions. The row counts and structure are stable. Use `(sym-name id)` to convert any intern ID back to its readable symbol name.
