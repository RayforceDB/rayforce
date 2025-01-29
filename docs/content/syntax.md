# :material-typewriter: Syntax Overview

## Basics

``` clj title="atoms"
;; bool
> true 
> false

;; char
> 'a'

;; i64

> 42

;; f64
> 42.1
> 42.

;; symbol
> abcdef ;; symbol
> 'abcdef ;; quoted symbol

;; timestamp
> 2021.01.01
> 2021.01.01D00:01:02.000000001
```

## Compound

``` clj title="vectors"
;; I64
> [1 2 3 4]

;; F64
> [1 2 3.3 4.4]

;; Symbol
> [a b c d]

;; Timestamp
> [2021.01.01 2021.01.02 2021.01.03]

;; String
> "Hello, world!"
```

``` clj title="lists"
;; List
↪ (list 1 "2" {a: 3 b: 4} 5)
(
  1
  "2"
  {a: 3 b: 4}
  5
)
```

``` clj title="dictionaries"
;; Dict
> {a: 1 b: 2 c: [1 2 3]}
{
  a: 1
  b: 2
  c: [1 2 3]
}
```

``` clj title="tables"
;; Table
↪ (table [a b c] (list [AA BB CC DD] (list 1 2 3 4) 8.9))
| a  | b | c    |
+----+---+------+
| AA | 1 | 8.90 |
| BB | 2 | 8.90 |
| CC | 3 | 8.90 |
| DD | 4 | 8.90 |
```

## Guid

Guid has not it's literal representation, but it can be created with `guid` function or parsed from string:

``` clj
↪ (guid 2)
[9c782460-ec88-f4f0-3c98-c4808ca89410 af72b934-d3c6-7da8-375a-815cdb2ec550]
↪ (first (guid 1))
9c782460-ec88-f4f0-3c98-c4808ca89410
↪ (as 'guid "9c782460-ec88-f4f0-3c98-c4808ca89410")
9c782460-ec88-f4f0-3c98-c4808ca89410
```

## Comments

``` clj
;; This is a comment
```

## User defined functions

``` clj
↪ (set f (fn [x] (+ 1 x)))
(fn [x] ..)
↪ (f 2)
3
```

## Expressions

Expressions uses a prefix notation, where the function is placed before its operands. For example, the expression `1 + 2` is written as `(+ 1 2)` in Rayforce. Each function belongs to a one of three types by its arity: unary, binary, and vary. Unary functions take one argument, binary functions take two arguments, and vary functions take any number of arguments. For example, the `avg` function is the unary function, the `+` function is a binary function, and the `list` function is a vary function.

``` clj
↪ (+ 1 2)
3
↪ (- 1 2)
-1
↪ (avg [1 2 3])
2
↪ ((fn [x] (+ 1 x)) 2)
3
↪ (set f (fn [x] (+ 1 x)))
(fn [x] ..)
↪ (f 2)
3
```
