# :material-repeat: Iterable Operations

RayforceDB provides functions for working with collections like [:material-code-array: Lists](../data-types/list.md) and [:material-vector-line: Vectors](../data-types/vector.md). These functions enable efficient data transformations, filtering, and element access.

## Element Access

### :material-cursor-pointer: At

Returns the element at the specified index or key from a collection.

```clj
(at [1 2 3 4 5] 2)
3

(at (list "Alice" "Bob" "Charlie") 1)
"Bob"

(set d (dict ['a 'b 'c] [1 2 3]))
(at d 'b)
2

(set t (table [name age] (list (list "Alice" "Bob") [25 30])))
(at t 'name)
(Alice Bob)
```

!!! note "Table Column Access"
    When accessing tables, use `at` with quoted symbols for column names.

### :material-arrow-left-bold: First

Returns the first element of a collection.

```clj
(first [1 2 3 4 5])
1

(first [150.25 300.50 125.75])
150.25

(first "hello")
'h'
```

### :material-arrow-right-bold: Last

Returns the last element of a collection.

```clj
(last [1 2 3 4 5])
5

(last [150.25 300.50 125.75])
125.75

(last "hello")
'o'
```

### :material-format-list-numbered: Take

Returns the first `n` elements of a collection. If `n` is negative, returns the last `n` elements.

```clj
(take 3 [1 2 3 4 5])
[1 2 3]

(take -3 [1 2 3 4 5])
[3 4 5]

(take 2 [150.25 300.50 125.75 200.00])
[150.25 300.50]

(take 2 'a')
"aa"
```

## Set Operations

### :material-minus-circle-outline: Except

Returns elements from the first collection that are not in the second collection.

```clj
(except [1 2 3 4 5] [2 4])
[1 3 5]

(except ['AAPL 'MSFT 'GOOG] ['AAPL 'GOOG])
[MSFT]

(except [1 2 3 4 5] 3)
[1 2 4 5]
```

### :material-vector-union: Union

Returns the distinct union of two collections, containing all unique elements from both.

```clj
(union [1 2 3] [4 5 6])
[1 2 3 4 5 6]

(union [1 2 3] [2 3 4])
[1 2 3 4]

(union ['AAPL 'MSFT] ['MSFT 'GOOG 'TSLA])
[AAPL MSFT GOOG TSLA]
```

### :material-vector-intersection: Sect

Returns the distinct intersection of two collections, containing only elements that appear in both.

```clj
(sect [1 2 3 4 5] [3 4 5 6 7])
[3 4 5]

(sect ['AAPL 'MSFT 'GOOG] ['MSFT 'GOOG 'TSLA])
[MSFT GOOG]

(sect [150.25 300.50 125.75] [300.50 200.00 125.75])
[300.50 125.75]
```

## Membership and Search

### :material-check-circle: In

Checks if an element exists in a collection. Returns a boolean or boolean vector. When the first argument is a collection, returns a boolean vector indicating membership for each element.

```clj
(in 2 [1 2 3])
true

(in 5 [1 2 3])
false

(in [1 2] [1 2 3 4 5])
[true true]

(in 'AAPL ['AAPL 'MSFT 'GOOG])
true

(in 'e' "test")
true
```

### :material-magnify: Find

Returns the index of the first occurrence of a value in a collection, or `null` if not found.

```clj
(find [1 2 3 4 5] 3)
2

(find [1 2 3 4 5] 6)
0Nl

(find ['AAPL 'MSFT 'GOOG] 'MSFT)
1

(find [150.25 300.50 125.75] 300.50)
1
```

### :material-filter: Filter

Filters a collection using a boolean mask, returning only elements where the corresponding mask value is `true`.

```clj
(filter [1 2 3 4 5] [true false true false true])
[1 3 5]

(filter "test" [true false true false])
"ts"

(filter ['AAPL 'MSFT 'GOOG 'TSLA] [true false false true])
[AAPL TSLA]

(set prices [150.25 300.50 125.75 200.00])
(filter prices [true false true true])
[150.25 125.75 200.00]
```

### :material-border-inside: Within

Checks if elements are within a range specified by a 2-element vector `[min max]`. Returns a boolean vector.

```clj
(within [6] [1 20])
[false]

(within [1 2 3] [1 100])
[true true true]

(within [5 15 25] [10 20])
[false true false]

(set prices [150 300 125 200])
(within prices [150 250])
[true false false true]
```

## Transformation Functions

### :material-apple-keyboard-caps: Apply

Applies a function to corresponding elements from multiple lists, using each list as an argument source.

```clj
(apply + [1 2 3] [4 5 6])
[5 7 9]

(apply + [1 2 3] [10 20 30] [100 200 300])
[111 222 333]

(apply + 1 [1 2 34])
[2 3 35]

(apply * [2 3 4] [5 6 7])
[10 18 28]

(set prices [150.25 300.50 125.75])
(set quantities [100 200 150])
(apply * prices quantities)
[15025.00 60100.00 18862.50]
```

### :material-arrow-right: Map

Applies a function to each element of a list and returns a new list with the results. The function is applied sequentially.

```clj
(map + 1 [1 2 3])
[2 3 4]

(map * 2 [1 2 3])
[2 4 6]

(map + 5 [4 5 6])
[9 10 11]

(set prices [150.25 300.50 125.75])
(map + 10 prices)
[160.25 310.50 135.75]

(map * 1.1 prices)
[165.28 330.55 138.33]
```

### :material-speedometer: Pmap

Applies a function to each element of a list **in parallel** using the thread pool and returns a new list with the results.

```clj
(pmap + 1 [1 2 3])
[2 3 4]

(pmap * 2 [1 2 3])
[2 4 6]

(set prices [150.25 300.50 125.75])
(pmap + 10 prices)
[160.25 310.50 135.75]
```

!!! tip "Performance"
    Use `pmap` for computationally intensive operations on large collections to leverage parallel processing.

### :octicons-fold-16: Fold

Performs a traditional left fold (reduce) operation, accumulating elements from a vector using a function. The function must accept two arguments: the accumulator and the current element.

```clj
(fold max [150.25 300.50 125.75])
300.50

(fold max [1 2 3 4 5])
5

(fold min [150.25 300.50 125.75])
125.75
```

## Dictionary and Table Operations

### :material-key: Key

Returns the keys from a mappable object like a [:material-code-braces: Dictionary](../data-types/dictionary.md) or [:material-table: Table](../data-types/table.md). For other types, returns the value itself.

```clj
(key (dict ['a 'b] [1 2]))
[a b]

(set t (table [name age] (list (list "Alice" "Bob") [25 30])))
(key t)
[name age]

(key [1 2])
[1 2]
```

### :material-code-tags: Value

Returns the values from a mappable object like a [:material-code-braces: Dictionary](../data-types/dictionary.md) or [:material-table: Table](../data-types/table.md). For [:material-code-braces: Enum](../data-types/enum.md) types, returns the underlying symbol vector. For other types, returns the value itself.

```clj
(value (dict ['a 'b] [1 2]))
[1 2]

(set t (table [name age] (list (list "Alice" "Bob") [25 30])))
(value t)
((Alice Bob) [25 30])

(value [1 2])
[1 2]
```
