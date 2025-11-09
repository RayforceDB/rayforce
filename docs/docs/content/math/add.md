
# Add `+`

Makes an addition of its arguments. Supports integers, floats, temporal types (date, time, timestamp), and their combinations.

## Basic Arithmetic

``` clj
↪ (+ 1 2)
3
↪ (+ 1.2 2.2)
3.4
↪ (+ 1 3.4)
4.4
↪ (+ [1 2 3] 3)
[4 5 6]
↪ (+ [1 2 3] 3.1)
[4.1 5.1 6.1]
↪ (+ 3.1 [1 2 3])
[4.1 5.1 6.1]
```

## Temporal Types

```clj
↪ (+ 2024.03.20 5)
2024.03.25
↪ (+ 2024.03.20 20:15:03.020)
2024.03.20D20:15:03.020000000
↪ (+ 20:15:07.000 60000)
20:16:07.000
↪ (+ 10:15:07.000 05:41:47.087)
15:56:54.087
↪ (+ 2025.03.04D15:41:47.087221025 1000000000i)
2025.03.04D15:41:48.087221025
```

## Null Handling

```clj
↪ (+ 0Ni 0Ni)
0Ni
↪ (+ 0Nf 5)
0Nf
↪ (+ 0.00 0Ni)
0Nf
```

!!! info
    - **Numeric types**: `i32` (int32), `i64` (int64), `f64` (float64)
    - **Temporal types**: `date`, `time`, `timestamp`
    - **Null values**: `0Ni` (null int32), `0Nl` (null int64), `0Nf` (null float64)
    - Adding `date + int` returns a date offset by days
    - Adding `date + time` returns a timestamp
    - Adding `time + time` returns a time
    - Adding `time + int` adds milliseconds
    - Adding `timestamp + int` adds nanoseconds
    - Any operation with null values returns a null of the appropriate type

!!! tip
    Use `+` for date arithmetic, time calculations, and offsetting temporal values
