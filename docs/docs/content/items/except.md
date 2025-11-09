# Except `except`

Returns the items in the first collection that are not in the second collection.

## Basic Usage

```clj
↪ (except [1 2 3 4 5] [2 4])
[1 3 5]
↪ (except ['a 'b 'c] ['a 'c])
[b]
↪ (except [1 2 3] [5 6 7])
[1 2 3]
```

## Scalar Except

```clj
↪ (except [1 2 3 4 5] 3)
[1 2 4 5]
↪ (except ['a 'b 'c] 'b)
[a c]
```

## Handling Duplicates

```clj
↪ (except [1 1 2 2 3] [1 3])
[2 2]
↪ (except ['a 'a 'b 'c 'c] ['a 'c])
[b]
```

!!! info
    - Returns elements from the first collection that don't exist in the second
    - Can use a scalar as the second argument to remove a single value
    - **Duplicates are preserved** in the result (unlike `distinct`)
    - If an element appears multiple times in the first collection but is excluded, all occurrences are removed
    - Empty collections are handled gracefully: `(except [] x)` returns `[]`, `(except x [])` returns `x`

!!! tip
    Use `except` to filter out unwanted values from a collection while preserving duplicates
