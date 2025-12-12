# :material-code-braces: Compose Operations

RayforceDB provides functions for creating, combining, and transforming data structures. These functions enable data construction and type conversion.

## Type Conversion

### :material-code-tags: Casting `as`

Converts a value to a specified type. Takes a type symbol and a value to convert.

```clj
(as 'I64 3.14)
3

(as 'F64 5)
5.0

(as 'String 42)
"42"

(as 'Symbol "hello")
'hello

;; Convert price to integer
(as 'I64 150.75)
150
```

!!! note ""
    The first argument must be a symbol representing a type name (see [Data Types](../data-types/overview.md))

## Collection Construction

### :material-plus-circle: Concat

Concatenates two tables, vectors or strings, combining them into a single table, vector or string.

```clj
(concat [1 2] [3 4])
[1 2 3 4]

(concat "hello" "world")
"helloworld"

(concat ['AAPL 'MSFT] ['GOOG 'TSLA])
[AAPL MSFT GOOG TSLA]

(concat [150.25 300.50] [125.75 200.00])
[150.25 300.50 125.75 200.00]
```

### :material-layers-triple: Raze

Flattens a list of vectors into a single vector, or flattens nested lists.

```clj
(raze (list [1 2] [3 4] [5 6]))
[1 2 3 4 5 6]

(raze (list (list 1 2) (list 3 4)))
(1 2 3 4)

(raze (list ['AAPL 'MSFT] ['GOOG 'TSLA]))
[AAPL MSFT GOOG TSLA]
```

### :material-format-list-numbered: Til

Generates a vector of integers from 0 to n-1.

```clj
(til 5)
[0 1 2 3 4]

(til 0)
[]

(til 10)
[0 1 2 3 4 5 6 7 8 9]
```

### :material-dice-6: Rand

Generates a vector of random integers between 0 (inclusive) and an upper bound (exclusive).
Takes two integer arguments: count and upper bound. Returns a vector of random integers in the range `[0, upper_bound)`.

```clj
(rand 5 10)
[3 7 2 9 1]

(rand 3 100)
[42 15 87]

(rand 10 1000)
[234 567 123 890 456 789 012 345 678 901]
```

## Collection Transformation

### :material-arrow-left-right: Reverse

Reverses the order of elements in a vector or list. Returns a new vector or list with elements in reverse order.

```clj
(reverse [1 2 3 4])
[4 3 2 1]

(reverse "hello")
"olleh"

(reverse (list "a" "b" "c"))
("c" "b" "a")

(reverse [150.25 300.50 125.75])
[125.75 300.50 150.25]
```


### :material-filter-variant: Distinct

Returns a vector containing only unique values from the input, preserving order of first occurrence.

```clj
(distinct [1 2 1 3 2 1])
[1 2 3]

(distinct ['AAPL 'MSFT 'AAPL 'GOOG 'MSFT])
[AAPL MSFT GOOG]

(distinct [150.25 300.50 150.25 125.75 300.50])
[150.25 300.50 125.75]
```


### :material-group: Group

Groups elements of a vector by value, returning a dictionary mapping unique values to lists of indices where they occur.

```clj
(group [1 2 1 3 2 1])
{1: (0 2 5), 2: (1 4), 3: (3)}

(group ['AAPL 'MSFT 'AAPL 'GOOG 'MSFT])
{AAPL: (0 2), MSFT: (1 4), GOOG: (3)}

(group [150.25 300.50 150.25 125.75 300.50])
{150.25: (0 2), 300.50: (1 4), 125.75: (3)}
```
