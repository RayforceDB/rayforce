
# Sub `-`

Makes a subtraction of its arguments. Supports integers, floats, temporal types (date, time, timestamp), and their combinations.

## Basic Arithmetic

``` clj
↪ (- 1 2)
-1
↪ (- 1.2 2.2)
-1.0
↪ (- 1 3.4)
-2.4
↪ (- [1 2 3] 3)
[-2 -1 0]
↪ (- [1 2 3] 3.1)
[-2.1 -1.1 -0.1]
↪ (- 3.1 [1 2 3])
[2.1 1.1 0.1]
```

## Temporal Types

```clj
↪ (- 2024.03.20 5)
2024.03.15
↪ (- 2024.03.20 2023.02.07)
407
↪ (- 2024.03.20 20:15:03.020)
2024.03.19D03:44:56.980000000
↪ (- 20:15:07.000 60000)
20:14:07.000
↪ (- 10:15:07.000 05:41:47.087)
04:33:19.913
↪ (- 2025.03.04D15:41:47.087221025 1000000000i)
2025.03.04D15:41:46.087221025
↪ (- 2025.03.04D15:41:47.087221025 2025.03.04D15:41:47.087221025)
0
```

!!! info
    - **Numeric types**: `i32` (int32), `i64` (int64), `f64` (float64)
    - **Temporal types**: `date`, `time`, `timestamp`
    - Subtracting `date - int` returns a date offset by days
    - Subtracting `date - date` returns the difference in days as int64
    - Subtracting `date - time` returns a timestamp
    - Subtracting `time - time` returns a time duration
    - Subtracting `time - int` subtracts milliseconds
    - Subtracting `timestamp - int` subtracts nanoseconds
    - Subtracting `timestamp - timestamp` returns the difference in nanoseconds

!!! tip
    Use `-` to calculate date differences, time durations, and offset temporal values backward
