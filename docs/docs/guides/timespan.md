# Timespan

TIMESPAN represents a signed 64-bit count of nanoseconds. It is a duration,
not a date or a time of day. One day is exactly 86,400 seconds; applying a
duration does not perform calendar or daylight-saving adjustments.

## Literals and conversions

```lisp
1D                            ; one day
0D01:00:00                    ; one hour
-0D00:00:00.000000001         ; minus one nanosecond
0Nn                           ; null
0Wn                           ; maximum signed count
-0Wn                          ; negative maximum signed count
(as 'I64 1D)                  ; 86400000000000
(as 'TIMESPAN 1000000000)      ; 0D00:00:01.000000000
(as 'TIMESPAN 01:00:00.000)    ; 0D01:00:00.000000000
(as 'TIMESPAN "1D01:00:00")    ; 1D01:00:00.000000000
```

The syntax is `[-]daysD[hh[:mm[:ss[.fraction]]]]`. Hours may exceed 23
(`0D24:00:00` is `1D`); minute and second fields must be between 00 and 59.
Compact hour/minute/second forms are also accepted. The day field has at
most six digits. A fraction is padded or truncated to nine decimal places. The initial minus
sign negates the whole duration. Printing uses the full day/time form,
except for null and the signed endpoint spellings, so values read back with
their type and nanosecond precision. Integer arithmetic wraps in 64 bits;
`INT64_MIN` is reserved for null. Floating casts round to the nearest
integer count, with half counts rounded away from zero.

Vectors use per-element duration syntax, for example `[1D 0Nn -1D]`.
`as 'TIMESPAN` also converts a vector element by element. TIME converts
milliseconds to nanoseconds exactly. Casting a timestamp to TIMESPAN
extracts its time of day; casting a timespan to I64 exposes its raw count.

## Arithmetic and result types

| Expression | Result |
|---|---|
| `timestamp - timestamp` | TIMESPAN |
| `timestamp + span`, `timestamp - span`, `span + timestamp` | TIMESTAMP |
| `span + span`, `span - span`, `neg span`, `abs span` | TIMESPAN |
| `span + integer`, `integer + span`, `span - integer` | TIMESPAN; integer counts nanoseconds |
| `span * integer`, `integer * span` | TIMESPAN |
| `span div integer`, `span div span` | TIMESPAN |
| `span / span`, `span / integer` | F64 |
| `span % integer` | TIMESPAN |
| `span % span`, `span * span` | Type error |
| Comparisons with another span or an integer count | BOOL |
| `sum spans` | TIMESPAN; skips nulls |
| `avg spans` | F64 nanoseconds; skips nulls |
| `min`, `max`, `first`, `last` | Preserve TIMESPAN |

On empty or all-null duration vectors, `min` returns `0Wn` and `max`
returns `-0Wn`.

F64 operands promote ordinary duration addition and multiplication to F64
nanosecond arithmetic. F32 operands are rounded to an integer count for
these duration operations. Integer division preserves the left operand's
type: a span divided by a span is a duration containing the quotient count,
not a dimensionless integer. Division floors toward negative infinity.
For example, `(div -0D00:00:00.000000001 2)` is minus one nanosecond, and
`(% -0D00:00:00.000000001 1000000000)` is 999,999,999 nanoseconds.

Nulls propagate through arithmetic. Integer division, remainder, and duration bucket widths of zero return a
typed null. F64 results retain the engine's existing
policy of converting non-finite results to `0Nf`.

`xbar` accepts a TIMESPAN bucket width for timestamps:

```lisp
(xbar 2024.01.15D12:34:56.000000000 0D01:00:00)
; 2024.01.15D12:00:00.000000000
```

Scalar, vector and query column expressions use the same duration arithmetic.
Ordinary, encoded and partitioned storage preserve duration types and nulls.
Explicit CSV schemas accept TIMESPAN and CSV writes use its literal syntax.
Parquet duration and clock-time annotations are not remapped by this change.

## Migrating timestamp differences

Timestamp subtraction previously returned I64. It now returns TIMESPAN,
including in queries and updates. To retain the previous integer-count
behavior, explicitly cast the difference before further arithmetic:

```lisp
(as 'I64 (- finish start))
(div (as 'I64 (- finish start)) 1000000000)
```

For a typed period, `(div (- finish start) period)` returns a TIMESPAN
holding the quotient count. Cast that quotient to I64 when it represents a
bar index. Use an integer nanosecond width for remainder, because two
TIMESPAN operands are not admitted by `%`:

```lisp
(as 'I64 (div (- finish start) period))
(% (- finish start) (as 'I64 period))
```

## Reference checks

The installed q 5.0 binary (2026.07.23) was used to generate 65 scalar cases for
`test/rfl/temporal/timespan_operators.rfl`. The fixture checks both result
types and raw counts for parsing, endpoint spellings, mixed arithmetic,
division, remainder, comparisons, timestamp subtraction and aggregates.
These checks establish the tested duration behavior; the existing F64
non-finite policy described above still applies.
