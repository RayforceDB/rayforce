# `.time.*` — clock, timers and sleep

Monotonic clock access, a recurring-callback scheduler, and a blocking sleep for scripts. Timers integrate into the runtime's poll loop — they only fire when control is back in the loop, i.e. between REPL inputs (interactive or piped: both read stdin through the loop), between IPC requests, or while idling in server-only mode. A long-running synchronous evaluation will defer pending timers until it returns. A script, or a piped session that reached end of input, stays alive until its pending timers are spent, so a one-shot timer set by a script fires before the process exits; a periodic timer keeps it running until the script ends the process itself. An open client handle does not keep a script alive.

The clock is the same `CLOCK_MONOTONIC` source the scheduler uses for its deadlines, so `(.time.now)` is directly comparable to the `ms` argument passed to `.time.timer.set`. Wall-clock time is intentionally not exposed here: a host sleep / resume must not retroactively shift a scheduled deadline.

!!! note "Restricted under `-U`"
    `.time.timer.set` and `.time.timer.del` are `RAY_FN_RESTRICTED` — installing callbacks on the server's poll loop is privileged. `.time.sleep` is restricted too: it stops the whole server while it waits, as `.sys.exec "sleep 1"` would, and that is restricted. `.time.now` is unrestricted (it's a clock read).

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.time.now`](#time-now) | variadic | — | Current monotonic time in milliseconds. |
| [`.time.timer.set`](#time-timer-set) | variadic | restricted | Schedule a callback; return its timer id. |
| [`.time.timer.del`](#time-timer-del) | unary | restricted | Cancel a scheduled timer (idempotent). |
| [`.time.sleep`](#time-sleep) | unary | restricted | Block the calling thread for a number of milliseconds; return null. |

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

Signature: `(.time.timer.del id)`. Removes the timer with that id from the heap and releases its retained lambda. Returns null. Idempotent — passing an unknown id is not an error. A timer may delete itself from inside its own callback: the current call finishes and the timer is not fired again.

Errors: `type` if `id` isn't an `i64`.

```lisp
(.time.timer.del id)        ;; cancel a running timer
(.time.timer.del id)        ;; second call: null, not an error
(.time.timer.del 999999)    ;; unknown id: null
```

## `.time.sleep` { #time-sleep }

Signature: `(.time.sleep ms)`. Blocks the calling thread for `ms` milliseconds, then returns null. It is the way for a script (`rayforce script.rfl`, or one piped into `rayforce -i`) to wait: enable something, wait, read the result back.

- `ms` — an integer atom of any width (`i64`, `i32`, `i16`, `u8`), in milliseconds, or a `timespan`, which is the duration itself, rounded up to the next whole millisecond.
- `0` (or a zero `timespan`) returns at once.
- The sleep never ends early: it waits until the monotonic clock has moved on by at least the time asked for. It can end later on a loaded machine.
- A value too large for the clock (anything past about 292 years) waits until it is cancelled. In an async IPC message it is a `domain` error instead (see [On a server](#time-sleep-server)).

Errors:

- `domain` — a negative duration, a null one (`0Nl`, `0Ni`, `0Nh`, `0Nn`), or one too large for the clock in an async IPC message.
- `type` — anything else: a float, a bool, a string or symbol, a date, time or timestamp, a vector, a list or a dict.
- `cancel` — the wait was cancelled, or the client of a sync request hung up (below).
- `access` — called by a restricted peer under `-U`.

### Ending a sleep early

The sleep waits in steps of at most 10 ms, and after each one checks the interrupt flag, as long-running builtins do between chunks of work. What a Ctrl-C does depends on where the sleep runs:

| Where | Ctrl-C |
|---|---|
| A sleep typed at the interactive REPL (a terminal) | Ends the sleep within about 10 ms with a `cancel` error, and the prompt comes back. |
| Script (`rayforce script.rfl`), or input piped into `rayforce -i` | No handler is installed, so the signal stops the process (exit status 130). The rest of the script does not run. |
| A client's sync `.ipc.send` | The client sends the server a cancel, which ends the sleep within about 10 ms. The client gets the `cancel` error, and the connection stays usable. |
| A client's async `.ipc.post` | The client cannot cancel an async message. |
| The server's own terminal, while it runs a client's request | A Ctrl-C typed there does nothing until the request ends: between its own inputs the REPL keeps the terminal in raw mode, where Ctrl-C is a key, not a signal, and the loop that would read that key is the one that is busy. Send the signal instead: `kill -INT <server pid>` cancels the running request, sync or async, within about 10 ms. A server without a terminal (server-only mode, or stdin not a terminal) installs no handler, so `kill -INT` stops that server. |

### On a server { #time-sleep-server }

A sleeping server answers nobody. It evaluates one request at a time, so until the sleep ends, every other connection, timer and hook waits, and new connections wait for their handshake.

- **Sync request** (`.ipc.send`): the sleep also ends when its client hangs up, since nobody is left waiting for the answer. The server then serves its other connections. On Linux the hang-up is seen within about 10 ms. Elsewhere, a client that dies after sending more requests behind the sleep is noticed only when the sleep ends. A client that only shuts down its sending side while it waits for the reply counts as hung up, because from the server's side the two look the same.
- **Async message** (`.ipc.post`): the sleep runs its full time, even after the sender has gone. A client that posts a sleep and then more work, then exits, still gets that work run after the whole delay. Only a server-side interrupt (`kill -INT` on a server with a terminal) ends it early. Because nothing on the client side could ever end it, an async sleep too large for the clock is a `domain` error instead of waiting forever.

### Journal replay

With `-l` or `-L`, a sync request is written to the journal before it runs, so a sleep sent with `.ipc.send` is logged like any other request. A sleep changes no state. During a replay (at startup, or from `.log.replay`) it returns null at once, so a logged sleep, even an endless one, neither delays a restart nor stops it.

```lisp
(.time.sleep 500)                    ;; half a second
(.time.sleep 0D00:00:02.000000000)   ;; two seconds, as a timespan

;; profile a running service for 20 s: enable, wait, read back
(set h (.ipc.open "127.0.0.1:5000"))
(.ipc.send h "(.sys.profile.enable 1)")
(.time.sleep 20000)
(.ipc.send h "(.sys.profile)")
```

!!! warning "A sleep stalls the whole server"
    `.time.sleep` holds no lock, but a server evaluates one request at a time. While one request sleeps, the server answers nobody: every other connection's requests, timers and IPC hooks wait until it returns, as they wait behind any long query. A client that may run `.time.sleep` can therefore stall the server for every client. A sync sleep lasts as long as it asks and its client stays connected. An async one lasts its full time, even after its sender has gone. That is why it is restricted under `-U`: a restricted peer gets `access`, and only a full-privilege client can stall a server this way.

    Inside a server, schedule the work with [`.time.timer.set`](#time-timer-set) instead. It returns at once and lets the loop serve other connections until the callback is due. Timers of the sleeping process do not fire during the sleep; they fire once control is back in the poll loop.

## Notes on the scheduler

- The min-heap is per `ray_poll_t`; one timer set per runtime.
- Ties on `exp_ms` are broken by `id` (lower id fires first).
- The heap is allocated lazily on the first `.time.timer.set` call — callers that never schedule pay nothing.
- On runtime shutdown the heap is walked and every retained lambda is released.
- There's no introspection (no `list-timers`, no `pause`/`resume`) — design choice; track ids yourself if you need to manage many.
