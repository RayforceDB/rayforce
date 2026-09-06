# `.time.*` — clock and timers

Monotonic clock access plus a recurring-callback scheduler. Timers integrate into the runtime's poll loop — they only fire when control is back in `ray_poll_run`, i.e. between REPL inputs, between IPC requests, or while idling in server-only mode. A long-running synchronous evaluation will defer pending timers until it returns.

The clock is the same `CLOCK_MONOTONIC` source the scheduler uses for its deadlines, so `(.time.now)` is directly comparable to the `ms` argument passed to `.time.timer.set`. Wall-clock time is intentionally not exposed here: a host sleep / resume must not retroactively shift a scheduled deadline.

!!! note "Restricted under `-U`"
    `.time.timer.set` and `.time.timer.del` are `RAY_FN_RESTRICTED` — installing callbacks on the server's poll loop is privileged. `.time.now` is unrestricted (it's a clock read).

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.time.now`](#time-now) | variadic | — | Current monotonic time in milliseconds. |
| [`.time.timer.set`](#time-timer-set) | variadic | restricted | Schedule a callback; return its timer id. |
| [`.time.timer.del`](#time-timer-del) | unary | restricted | Cancel a scheduled timer (idempotent). |

## `.time.now` { #time-now }

Signature: `(.time.now)`. Returns an `i64` count of milliseconds since some monotonic epoch (platform-defined; comparable only against itself and against timer deadlines on the same process).

Errors: `domain` if called with any arguments.

```lisp
(set t0 (.time.now))
(sum (til 1000))
(- (.time.now) t0)
;; => elapsed ms
```

## `.time.timer.set` { #time-timer-set }

Signature: `(.time.timer.set ms num fn)`.

- `ms` — `i64` ≥ 0. The delay until the first fire **and** the interval between subsequent fires.
- `num` — `i64` ≥ 0. **`0` means fire forever**; `N ≥ 1` means fire exactly N times.
- `fn` — a `RAY_LAMBDA` whose declared parameter list has exactly one entry. The callback is invoked with the current monotonic time (the same value `.time.now` would return).

Returns the new timer's `i64` id. IDs are monotonically increasing per runtime — never reused after deletion.

Errors:

- `domain` — arity not 3, negative `ms`, negative `num`, lambda with arity ≠ 1, or no poll loop active.
- `type` — non-`i64` `ms` / `num`, or `fn` not a lambda.
- `oom` — heap allocation failed.

The callback's return value is released and discarded — timers are side-effecting procedures. If the callback raises an error, the error is printed to `stderr` as `timer <id>: error: <code>: <message>` (the same line the REPL would print) and the timer continues as if the call had succeeded; one fire is still counted toward `num`.

A callback that overruns its period does not replay the fires it missed. After each fire the timer re-arms at its next scheduled deadline, or one period from now if that deadline has already passed, so a periodic timer fires at most once per `ms` and the poll loop gets back to serving I/O between fires.

```lisp
;; Print "tick" every second, three times
(set id (.time.timer.set 1000 3 (fn [t] (println "tick @" t))))

;; Forever (num=0)
(set heartbeat
     (.time.timer.set 5000 0
       (fn [t] (println "heartbeat"))))
```

## `.time.timer.del` { #time-timer-del }

Signature: `(.time.timer.del id)`. Removes the timer with that id from the heap and releases its retained lambda. Returns null. Idempotent — passing an unknown id is not an error.

Errors: `type` if `id` isn't an `i64`.

```lisp
(.time.timer.del id)        ;; cancel a running timer
(.time.timer.del id)        ;; second call: null, not an error
(.time.timer.del 999999)    ;; unknown id: null
```

## Notes on the scheduler

- The min-heap is per `ray_poll_t`; one timer set per runtime.
- Ties on `exp_ms` are broken by `id` (lower id fires first).
- The heap is allocated lazily on the first `.time.timer.set` call — callers that never schedule pay nothing.
- On runtime shutdown the heap is walked and every retained lambda is released.
- There's no introspection (no `list-timers`, no `pause`/`resume`) — design choice; track ids yourself if you need to manage many.
