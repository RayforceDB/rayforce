# Apply `apply`

Applies a function to corresponding elements from multiple lists, using each list as an argument source.

## Syntax

```clj
(apply function arg1 arg2 ...)
```

## Examples

### Element-wise operations on multiple lists

```clj
↪ (apply (fn [x y] (+ x y)) [1 2 3] [4 5 6])
[5 7 9]

↪ (apply (fn [x y z] (+ x y z)) [1 2 3] [10 20 30] [100 200 300])
[111 222 333]
```

### Broadcasting scalar with list

```clj
↪ (apply (fn [x y] (+ x y)) 1 [1 2 34])
[2 3 35]
```