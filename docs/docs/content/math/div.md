
# Div `/`

Makes an integer division of its arguments, taking the quotient. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

## Basic Division

``` clj
↪ (/ 1 2)
0
↪ (/ 1.2 2.2)
0.00
↪ (/ 1 3.4)
0
↪ (/ [-1 -2 -3] 3)
[-1 -1 -1]
↪ (/ [10 -10 3] 2.1)
[4 -5 1]
↪ (/ 3.1 [1 2 -3])
[3.00 1.00 -2.00]
```

## Division by Zero and Null Handling

```clj
↪ (/ 5 0)
0Nl
↪ (/ 6 0.00)
0Nl
↪ (/ 0Nl 5)
0Nl
↪ (/ 0Ni 5)
0Ni
↪ (/ 0Nf 5)
0Nf
↪ (/ 5 0Ni)
0Nl
```

!!! info
    - **Numeric types**: `i32` (int32), `i64` (int64), `f64` (float64)
    - **Null values**: `0Ni` (null int32), `0Nl` (null int64), `0Nf` (null float64)
    - Returns the quotient (integer part) of the division
    - Division by zero returns null (`0Nl`)
    - Division with any null value returns null
    - For floating-point division, use `fdiv` instead

!!! warning
    Division by zero does not throw an error, it returns a null value

!!! tip
    Use `/` for integer division and `fdiv` for floating-point division with decimal results
