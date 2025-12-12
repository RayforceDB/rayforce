# :material-exponent: Math Operations

RayforceDB provides comprehensive mathematical operations for working with numeric and temporal data types. These functions operate on [:material-vector-line: Vectors](../data-types/vector.md) and [:simple-scalar: Scalar](./overview.md).

## Binary Arithmetic Operations

### :material-plus: Add `+`

Performs addition on its arguments. Works with scalars, vectors, and temporal types.

```clj
(+ 1 2)
3

(+ [1 2 3] 3)
[4 5 6]

(+ [100 200 300] [10 20 30])
[110 220 330]

(+ 2024.03.20 5)
2024.03.25
```

### :material-minus: Subtract `-`

Performs subtraction on its arguments. Works with scalars, vectors, and temporal types.

```clj
(- 1 2)
-1

(- [1 2 3] 3)
[-2 -1 0]

(- [100 200 300] [10 20 30])
[90 180 270]

(- 2024.03.20 5)
2024.03.15
```

### :material-close: Multiply `*`

Performs multiplication on its arguments. Works with scalars, vectors, and temporal types.

```clj
(* 1 2)
2

(* [1 2 3] 3)
[3 6 9]

(* [100 200 300] [2 3 4])
[200 600 1200]

(* 3i 02:15:07.000)
06:45:21.000
```

### :material-division: Divide `/`

Performs integer division, returning the quotient.

```clj
(/ 10 3)
3

(/ [10 -10 3] 2)
[5 -5 1]

(/ [100 200 300] [2 4 5])
[50 50 60]
```

!!! warning "Division by Zero"
    Division by zero returns null (`0Nl`). For floating-point division, use the `div` function.

### :material-percent: Modulo `%`

Performs modulo operation, returning the remainder of division.

```clj
(% 10 3)
1

(% [10 -10 3] 3)
[1 2 0]

(% [100 200 300] [7 13 17])
[2 5 11]
```

## Aggregation Functions

Aggregation functions operate on [:material-vector-line: Vectors](../data-types/vector.md) and return scalar values.

### :material-sigma: Sum

Calculates the sum of values in a [:material-vector-line: Vector](../data-types/vector.md).

```clj
(sum [1 2 3])
6

(sum [1.2 2.2 3.2])
6.60

(sum [100 200 300 150])
750
```

### :material-function-variant: Avg

Calculates the average (mean) value of a [:material-vector-line: Vector](../data-types/vector.md).

```clj
(avg [1.0 2.0 3.0])
2.00

(avg [-24 12 6])
-2.00

(avg [150.25 300.50 125.75])
192.17
```

### :material-chart-line: Med

Calculates the median (middle value) of a [:material-vector-line: Vector](../data-types/vector.md).

```clj
(med [3 1 2])
2.00

(med [3 1 2 4])
2.50

(med [150 300 125 200])
175
```

!!! note ""
    The `med` function works with integer vectors. For floating-point values, convert to integers or use integer representations.

### :material-chart-bell-curve: Dev

Calculates the standard deviation of a [:material-vector-line: Vector](../data-types/vector.md).

```clj
(dev [1 2 3 4 50])
19.03

(dev [1i 2i])
0.5

(dev [150.25 300.50 125.75])
77.25
```

### :material-arrow-down: Min

Returns the minimum value in a [:material-vector-line: Vector](../data-types/vector.md).

```clj
(min [1 2 3])
1

(min [10.5 2.3 7.8])
2.3

(min [150.25 300.50 125.75])
125.75
```

### :material-arrow-up: Max

Returns the maximum value in a [:material-vector-line: Vector](../data-types/vector.md).

```clj
(max [1 2 3])
3

(max [10.5 2.3 7.8])
10.5

(max [150.25 300.50 125.75])
300.50
```

## Rounding Functions

### :material-arrow-up-bold: Ceil

Rounds a value up to the nearest integer greater than or equal to the given value.

```clj
(ceil 1.5)
2.00

(ceil [1.1 2.5 -1.1])
[2.00 3.00 -1.00]

(ceil [150.25 300.49 125.01])
[151.00 301.00 126.00]
```

### :material-arrow-down-bold: Floor

Returns the largest integer less than or equal to a given number.

```clj
(floor 1.5)
1.00

(floor [1.1 2.5 -1.1])
[1.00 2.00 -2.00]

(floor [150.25 300.49 125.99])
[150.00 300.00 125.00]
```

### :material-circle-outline: Round

Rounds a number to the nearest integer. If halfway between two integers, rounds away from zero.

```clj
(round 1.5)
2.00

(round [-1.5 -1.1 0.0 1.1 1.5])
[-2.00 -1.00 0.00 1.00 2.00]

(round [150.25 300.49 125.75])
[150.00 300.00 126.00]
```


## Special Functions

### :material-timer: Xbar

Rounds values down to the nearest multiple of a specified bin width.

```clj
(xbar 17 5)
15

(xbar [10 11 12 13 14] 3)
[9 9 12 12 12]

;; Round timestamps to 1-minute buckets (60000 milliseconds)
(xbar [09:00:15.000 09:01:30.000 09:02:45.000] 60000)
[09:00:00.000 09:01:00.000 09:02:00.000]

;; Round prices to nearest 5 increment
(xbar [152.30 157.80 163.20] 5)
[150.00 155.00 160.00]
```
