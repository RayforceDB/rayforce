# Fold `fold`

Performs a traditional left fold (reduce) operation, accumulating elements from a vector using a lambda function.

## Syntax

```clj
(fold lambda-fn vector)
```

## Example

```clj
↪ (fold (fn [acc x] (+ acc x)) [1 2 3 4 5])
15
```

The lambda function receives two arguments:
- `acc` - the accumulated value
- `x` - the current element

The accumulation proceeds: `((((1 + 2) + 3) + 4) + 5) = 15`

!!! info
    - Takes a single vector and returns a scalar accumulated result
    - The lambda performs left fold: processes elements from left to right
    - The first element becomes the initial accumulator value
    - Use a lambda function with two parameters: `(fn [acc x] ...)`

!!! tip
    For element-wise operations, use [`map`](map.md) instead

## See Also

- [`map`](map.md) - Apply a function element-wise to vectors
- [`apply`](apply.md) - Apply a function with multiple arguments
