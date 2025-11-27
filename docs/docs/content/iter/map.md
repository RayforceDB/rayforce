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
(
  1
  2
  3
)
```

### Mapping with operators

```clj
↪ (map + 5 [4 5 6])
[9 10 11]
```

### Mapping with built-in functions

```clj
↪ (map count (list (list "aaa" "bbb")))
[2]
```

### Nested mapping

```clj
;; Note: Nested lambdas don't capture outer scope variables (no closures)
;; This example does NOT work - 'x' is undefined in inner fn:
;; (map (fn [x] (map (fn [y] (+ x y)) l)) l)

;; For nested operations, use explicit indexing or restructure the logic
```

## Notes

- The function is applied to each element individually
- Works with any type of list or vector
- Commonly used with lambda functions defined with `fn`
- Returns a new list without modifying the original
