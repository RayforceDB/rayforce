
# Div `div`

Makes a floating-point division of its arguments. Always returns a float result. Supports such types and their combinations: `i32`, `i64`, `I64`, `f64`, `F64`.

## Basic Division

```clj
↪ (div 10i 5i)
2.0

↪ (div 3i 5i)
0.6

↪ (div -10i 5i)
-2.0

↪ (div -9i 5i)
-1.8

↪ (div 10i 5.0)
2.0

↪ (div -3i 5.0)
-0.6
```

## Vector Division

```clj
↪ (div [1 2 3] 3)
[0.33 0.67 1.00]

↪ (div [1 2 3] 2.1)
[0.48 0.95 1.43]

↪ (div 3.1 [1 2 3])
[3.10 1.55 1.03]
```

## Division by Zero and Null Handling

```clj
↪ (div 3i 0i)
0Nf

↪ (div -3i 0i)
0Nf

↪ (div 3i 0.0)
0Nf

↪ (div -3i 0.0)
0Nf
```

!!! info
    - **Numeric types**: `i32` (int32), `i64` (int64), `f64` (float64)
    - **Null values**: `0Nf` (null float64)
    - Always returns float, unlike `/` which performs integer division
    - Division by zero returns null (`0Nf`)
    - Division with any null value returns null

!!! warning
    Division by zero does not throw an error, it returns a null value

!!! tip
    Use `div` for floating-point division with decimal results, use `/` for integer division
