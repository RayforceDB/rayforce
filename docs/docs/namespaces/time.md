# `.time.*` — clock, timers and time zones

Monotonic clock access plus a recurring-callback scheduler. Timers integrate into the runtime's poll loop — they only fire when control is back in the loop, i.e. between REPL inputs (interactive or piped: both read stdin through the loop), between IPC requests, or while idling in server-only mode. A long-running synchronous evaluation will defer pending timers until it returns. A script, or a piped session that reached end of input, stays alive until its pending timers are spent, so a one-shot timer set by a script fires before the process exits; a periodic timer keeps it running until the script ends the process itself. An open client handle does not keep a script alive.

The clock is the same `CLOCK_MONOTONIC` source the scheduler uses for its deadlines, so `(.time.now)` is directly comparable to the `ms` argument passed to `.time.timer.set`. Wall-clock time is intentionally not exposed here: a host sleep / resume must not retroactively shift a scheduled deadline.

!!! note "Restricted under `-U`"
    `.time.timer.set` and `.time.timer.del` are `RAY_FN_RESTRICTED` — installing callbacks on the server's poll loop is privileged. `.time.now` is unrestricted (it's a clock read).

## Reference

| Function | Arity | Flags | Description |
|---|---|---|---|
| [`.time.now`](#time-now) | variadic | — | Current monotonic time in milliseconds. |
| [`.time.timer.set`](#time-timer-set) | variadic | restricted | Schedule a callback; return its timer id. |
| [`.time.timer.del`](#time-timer-del) | unary | restricted | Cancel a scheduled timer (idempotent). |
| [`.time.tz.local`](#time-tz-local) | binary | — | A UTC timestamp on a time zone's wall clock. |
| [`.time.tz.offset`](#time-tz-offset) | binary | — | A time zone's offset from UTC at a timestamp. |
| [`.time.tz.utc`](#time-tz-utc) | variadic | — | The UTC timestamp of a time zone's wall clock. |
| [`.time.tz.version`](#time-tz-version) | variadic | — | The release of the time zone database built in. |

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

## `.time.tz.local` { #time-tz-local }

Signature: `(.time.tz.local 'Zone ts)`. Reads `ts`, a `timestamp` atom or vector holding instants in UTC, on the wall clock of `Zone`, and returns a `timestamp` of the same shape. `Zone` is a symbol naming an [IANA time zone](https://www.iana.org/time-zones), such as `'America/New_York` or `'Asia/Kolkata`. The result carries no zone: it is the date and time a clock on the wall in that zone showed, so `trunc`, `date`, `hh` and `xbar` bucket it by local days and hours.

A null stays null. A wall clock past the `timestamp` range is null.

Errors: `type` if `Zone` is not a symbol or `ts` is not a timestamp; `domain` if the database has no zone of that name.

```lisp
(.time.tz.local 'America/New_York 2024.07.02D01:30:00.000000000)
;; 2024.07.01D21:30:00.000000000 — still 1 July in New York

;; average price per New York calendar day
(select {from: trades
         by: {day: (trunc 'day (.time.tz.local 'America/New_York ts))}
         px: (avg price)})

;; per local hour of the day, over many days
(select {from: trades
         by: {hour: (hh (.time.tz.local 'America/New_York ts))}
         n: (count price)})
```

Inside a `select` the call compiles to an operator of its own, so on a partitioned table a `by:` key built from it is computed partition by partition, and the table is not copied into memory first.

## `.time.tz.offset` { #time-tz-offset }

Signature: `(.time.tz.offset 'Zone ts)`. Returns how far `Zone` is from UTC at each instant of `ts`, as a `timespan` of the same shape, positive east of UTC. `(.time.tz.local z ts)` is `(+ ts (.time.tz.offset z ts))`.

Nulls and errors are as for `.time.tz.local`.

```lisp
(.time.tz.offset 'America/New_York 2024.01.01D12:00:00.000000000)   ;; -0D05:00:00
(.time.tz.offset 'America/New_York 2024.07.01D12:00:00.000000000)   ;; -0D04:00:00
(.time.tz.offset 'Asia/Kolkata     2024.07.01D12:00:00.000000000)   ;;  0D05:30:00
```

## `.time.tz.utc` { #time-tz-utc }

Signature: `(.time.tz.utc 'Zone 'earlier wall)` or `(.time.tz.utc 'Zone 'later wall)`. The other direction: `wall` is a `timestamp` atom or vector holding wall-clock readings in `Zone`, and the result is the instant, in UTC, at which the zone's clock showed each one.

A clock shows most readings once, and then the second argument changes nothing. Near a change of offset a reading has two candidate instants: the reading taken with the offset before the change, and with the offset after it. `'earlier` takes the earlier of the two and `'later` the later. That one rule covers both cases:

| | on the wall | `'earlier` | `'later` |
|---|---|---|---|
| Clocks go back: the hour happens twice | `2024.11.03D01:30` in New York | `2024.11.03D05:30`, the first pass | `2024.11.03D06:30`, the second pass |
| Clocks go forward: the hour never happens | `2024.03.10D02:30` in New York | `2024.03.10D06:30`, which the clock showed as `01:30` | `2024.03.10D07:30`, which the clock showed as `03:30` |

The choice is required and has no default, so the reading of such an hour is in the text of the call. The function answers an instant for every wall time: it has no null of its own, only a null for a null and for an instant past the `timestamp` range.

Errors: `type` if `Zone` or the choice is not a symbol or `wall` is not a timestamp; `domain` if the database has no zone of that name or the choice is neither `'earlier` nor `'later`.

```lisp
(.time.tz.utc 'America/New_York 'earlier 2024.07.01D08:00:00.000000000)   ;; 2024.07.01D12:00:00.000000000

;; the instant a New York calendar day starts at, to filter a UTC column by
(.time.tz.utc 'America/New_York 'earlier 2024.07.01D00:00:00.000000000)   ;; 2024.07.01D04:00:00.000000000

;; a wall time the clock never showed does not come back from its instant
(set w 2024.03.10D02:30:00.000000000)
(== w (.time.tz.local 'America/New_York (.time.tz.utc 'America/New_York 'earlier w)))   ;; false
```

`(.time.tz.utc z c (.time.tz.local z ts))` is `ts` for every instant under one choice or the other: under both outside a repeated hour, under `'earlier` in its first pass and `'later` in its second.

Unlike the two functions above, this one is an ordinary call inside a `select`, not an operator of its own.

## `.time.tz.version` { #time-tz-version }

Signature: `(.time.tz.version)`. Returns the release of the IANA database built into this binary, as a string such as `"2026f"`.

## Notes on time zones

- **The database is built in.** Every zone and old alias of the IANA database is compiled into the binary, about 230 KB, so the functions answer the same on every platform and need no files on the host. A zone's rules are those of the release `.time.tz.version` names. The database is refreshed with each Rayforce release (see `RELEASE.md`), so a rule a country changes takes effect here from the next release.
- **Before 1970, zones are merged.** The database is built the way distributions usually package it, without IANA's `backzone` file. Zones that have agreed since 1970 are one zone, and a zone's offsets before 1970 are those of the zone it was merged into. A host whose own database was built with `backzone` can answer differently for those years.
- **The two directions are not alike.** Every instant has exactly one wall-clock reading, so `.time.tz.local` is a function with nothing to choose. The reverse is not: where clocks go forward a wall-clock hour never happens, and where they go back an hour happens twice. `.time.tz.utc` therefore takes the choice as an argument.
- **Days are not all 24 hours.** A local day on which the clocks change holds 23 or 25 hours of rows. Averages per day are unaffected; sums and counts on that day are not comparable with its neighbours.
- **Hours across a clock change.** `(trunc 'hour (.time.tz.local z ts))` puts the two passes through a repeated hour into one bucket. To keep every bucket one real hour, bucket the instant, `(trunc 'hour ts)`, and show `.time.tz.local` of the bucket as its label; `.time.tz.offset` tells the two same-labelled buckets apart.
- **A table of changes per zone.** A zone is parsed once, on first use, into the list of instants its offset changes at, through to the end of the `timestamp` range. A lookup is a binary search in that list, skipped while consecutive rows fall between the same two changes, which is nearly always in a time-ordered column.

## Notes on the scheduler

- The min-heap is per `ray_poll_t`; one timer set per runtime.
- Ties on `exp_ms` are broken by `id` (lower id fires first).
- The heap is allocated lazily on the first `.time.timer.set` call — callers that never schedule pay nothing.
- On runtime shutdown the heap is walked and every retained lambda is released.
- There's no introspection (no `list-timers`, no `pause`/`resume`) — design choice; track ids yourself if you need to manage many.
