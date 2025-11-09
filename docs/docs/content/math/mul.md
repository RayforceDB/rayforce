
# Mul `*`

Makes a multiplication of its arguments. Supports integers, floats, time values, and their combinations.

## Basic Arithmetic

``` clj
↪ (* 1 2)
2
↪ (* 1.2 2.2)
2.64
↪ (* 1 3.4)
3.40
↪ (* [1 2 3] 3)
[3 6 9]
↪ (* [1 2 3] 3.1)
[3.10 6.20 9.30]
↪ (* 3.1 [1 2 3])
[3.10 6.20 9.30]
```

## Time Multiplication

```clj
↪ (* 3i 02:15:07.000)
06:45:21.000
↪ (* 00:15:07.000 6)
01:30:42.000
↪ (* 2i [20:15:07.000 15:41:47.087])
[40:30:14.000 31:23:34.174]
```

## Null Handling

```clj
↪ (* 0Ni 15:12:10.000)
0Nt
↪ (* 0 -5.50)
0.00
```

!!! info
    - **Numeric types**: `i32` (int32), `i64` (int64), `f64` (float64)
    - **Time type**: `time`
    - **Null values**: `0Ni` (null int32), `0Nl` (null int64), `0Nf` (null float64), `0Nt` (null time)
    - Multiplying `time * int` scales the time duration
    - Multiplying `int * time` also scales the time duration
    - Cannot multiply `time * time`
    - Any operation with null values returns a null of the appropriate type

!!! tip
    Use `*` to scale time durations or perform standard numeric multiplication
