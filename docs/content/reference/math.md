## Add `+`

Makes an addition of it's arguments. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`, `Timestamp`.

``` clj
> (+ 1 2)
3
> (+ 1.2 2.2)
3.4
> (+ 1 3.4)
4.4
> (+ [1 2 3] 3)
[4 5 6]
> (+ [1 2 3] 3.1)
[4.1 5.1 6.1]
> (+ 3.1 [1 2 3])
[4.1 5.1 6.1]
```

## Sub `-`

Makes a subtraction of it's arguments. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`, `Timestamp`.

``` clj
> (- 1 2)
-1
> (- 1.2 2.2)
-1
> (- 1 3.4)
-2
> (- [1 2 3] 3)
[-2 -1 0]
> (- [1 2 3] 3.1)
[-2.1 -1.1 -0.1]
> (- 3.1 [1 2 3])
[2.1 1.1 0.1]
```

## Mul `*`

Makes a multiplication of it's arguments. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

``` clj
> (* 1 2)
2
> (* 1.2 2.2)
2.64
> (* 1 3.4)
3.4
> (* [1 2 3] 3)
[3 6 9]
> (* [1 2 3] 3.1)
[3.1 6.2 9.3]
> (* 3.1 [1 2 3])
[3.1 6.2 9.3]
```

## Div `/`

Makes a division of it's arguments, takes a quotient. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

``` clj
> (/ 1 2)
0
> (/ 1.2 2.2)
0
> (/ 1 3.4)
0
> (/ [1 2 3] 3)
[0 0 1]
> (/ [1 2 3] 2.1)
[0 0 1]
> (/ 3.1 [1 2 3])
[3 1 1]
```

## Mod `%`

Makes a division of it's arguments, takes a remainder. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

``` clj
> (% 1 2)
1
> (% 1.2 2.2)
1
> (% 2 3.4)
2
> (% [1 2 3] 3)
[1 2 0]
> (% [1 2 3] 2.1)
[1 2 0]
> (% 3.1 [1 2 3])
[0 1 0]
```

## Fdiv `div`

Makes a division of it's arguments. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

``` clj
> (div 1 2)
0.5
> (div 1.2 2.2)
0.5454545454545454
> (div 1 3.4)
0.29411764705882354
> (div [1 2 3] 3)
[0.3333333333333333 0.6666666666666666 1]
> (div [1 2 3] 2.1)
[0.47619047619047616 0.9523809523809523 1.4285714285714286]
> (div 3.1 [1 2 3])
[3.1 1.55 1.0333333333333334]
```

## Sum `sum`

Makes a sum of it's argument. Supports such types: `i64`, `I64`, `f64`, `F64`.

``` clj
> (sum 3)
3
> (sum [1.2 2.2 3.2])
6.6
> (sum [1 2 3])
6
```
