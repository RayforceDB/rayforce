# `.mem.*` — memory measurement

Value-level memory inspection and explicitly scoped query measurement. These
operations are unrestricted and do not change the measured value.

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.mem.objsize`](#mem-objsize) | unary | — | Logical bytes retained by a value. |
| [`.mem.ts`](#mem-ts) | 1 | special form | Evaluate once and return result plus time, allocation, peak-memory, result-size, and worker statistics. |

## `.mem.objsize` { #mem-objsize }

Signature: `(.mem.objsize value)`.

Returns the logical byte size of the complete `ray_t` graph reachable from
`value`. Each object header and live payload is counted; a shared child is
counted once. The number deliberately excludes buddy-block slack, global
symbol dictionaries, and opaque native handles, making it stable across
allocator tuning. It is an in-memory size, not a serialized-wire size.

```lisp
(.mem.objsize 42)       ;; 32: one ray_t header
(.mem.objsize [1 2 3])  ;; 56: 32-byte header + three I64 cells
```

## `.mem.ts` { #mem-ts }

Signature: `(.mem.ts expression)`.

`.mem.ts` is a special form: it evaluates `expression` exactly once inside a
process-wide measurement scope. Secondary-worker allocations and worker busy
time are included. Result sizing and construction of the returned statistics
dictionary happen after the time/allocation scope closes.

```lisp
(set m (.mem.ts (sum (til 1000000))))
(at m 'result)
(at m 'peak-live-bytes)
```

The returned dictionary has these fields:

| Key | Meaning |
|---|---|
| `result` | The expression's result. |
| `time-ns` | Monotonic wall-clock duration. |
| `memory-bytes` | Net process-memory delta from the shared per-query profiler/query-log measurement. |
| `allocated-bytes` | Sum of allocator block bytes obtained during the scope, including temporary allocations. |
| `freed-bytes` | Sum of allocator block bytes released before the measured evaluation returned. |
| `net-bytes` | Allocated minus freed bytes at the measurement boundary; it may be negative when the expression releases pre-existing values. |
| `peak-live-bytes` | Maximum positive live-byte delta above the starting boundary. |
| `result-bytes` | Logical `.mem.objsize` of `result`. |
| `alloc-count` | Number of allocator blocks obtained. |
| `free-count` | Number of allocator blocks released. |
| `workers` | Pool workers, including worker 0 (the main thread), that executed at least one parallel task. Zero means no pool dispatch occurred. |
| `worker-busy-ns` | Busy nanoseconds summed across participating workers. |
| `parallelism` | `worker-busy-ns / time-ns`. |

Only one `.mem.ts` scope may be active in a process, so nested calls return a
`state` error. Allocation instrumentation is normally inactive; ordinary
queries pay only a predicted-not-taken check at allocator entry/exit. While a
scope is active, atomic accounting adds intentional overhead. For performance
benchmarks, time normal executions separately and use `.mem.ts` as an
additional memory-measurement execution.

The common fields (`time-ns`, `memory-bytes`, `workers`, `worker-busy-ns`, and
`parallelism`) come from the same per-query measurement lifecycle used by the
profiler and query log. The allocation counters and `result-bytes` are the
additional `.mem.ts`-specific detail.
