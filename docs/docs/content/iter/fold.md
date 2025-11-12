# Fold `fold`

Applies a function to vectors or lists. The behavior depends on the function type:

- **With binary operators** (`+`, `-`, `*`, etc.): Performs element-wise operations on two vectors of the same length
- **With lambda functions**: Performs traditional fold/reduce, accumulating all elements from all vectors

## Syntax

```clj
(fold operator vector1 vector2)       ; Element-wise
(fold lambda-fn vector1 ... vectorN)  ; Reduce/accumulate
```

## Element-wise operations (with binary operators)

When using binary operators, `fold` applies the operation element-wise to corresponding elements from two vectors.

```clj
↪ (fold + [1 2 3] [4 5 6])
[5 7 9]

↪ (fold * [1 2 3] [4 5 6])
[4 10 18]

↪ (fold - [10 20 30] [5 10 15])
[5 10 15]
```

## Reduce/accumulate (with lambda functions)

When using lambda functions, `fold` accumulates all elements from all input vectors into a single result.

```clj
↪ (fold (fn [x y] (+ x y)) [1 2 3] [4 5 6])
12

↪ (fold (fn [x y] (* x y)) [1 2 3] [4 5 6])
30

↪ (fold (fn [acc x] (+ acc x)) [1 2 3 4 5])
15
```

!!! info
    - **With operators**: Both vectors must have the same length, returns a vector
    - **With lambda**: All vectors must have the same length, returns a scalar accumulated result
    - The lambda receives elements from all vectors: first from all vectors at index 0, then all at index 1, etc.
    - For element-wise operations, the function is applied: `result[i] = f(vector1[i], vector2[i])`
    - For reduction, elements are accumulated across all input vectors

!!! warning
    All input vectors must have the same length, otherwise an error will be thrown

## See Also

- [`apply`](apply.md) - Similar element-wise application with different syntax
- [`map`](map.md) - Apply a unary function to each element
