# Map `map`

Applies a function to each element of a list and returns a new list with the results.

## Syntax

```clj
(map function list)
```

## Examples

### Basic mapping with lambda functions

```clj
↪ (map (fn [x] (+ x 1)) [1 2 3])
[2 3 4]

↪ (map (fn [x] (* x 2)) [1 2 3])
[2 4 6]

↪ (map (fn [x] (as 'String x)) [1 2 3])
["1" "2" "3"]
```

### Mapping with built-in functions

```clj
↪ (map count (list (list "aaa" "bbb")))
[2]
```

### Nested mapping

```clj
↪ (set l (list 1 2 3 4))
↪ (map (fn [x] (map (fn [y] (+ x y)) l)) l)
(list [2 3 4 5] [3 4 5 6] [4 5 6 7] [5 6 7 8])
```

## Notes

- The function is applied to each element individually
- Works with any type of list or vector
- Commonly used with lambda functions defined with `fn`
- Returns a new list without modifying the original
