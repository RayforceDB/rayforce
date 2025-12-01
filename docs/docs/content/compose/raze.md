# Raze `raze`

Flattens a list of lists/vectors into a single list or vector.

## Basic Usage

```clj
↪ (raze (list [1 2] [3 4]))
[1 2 3 4]

↪ (raze (list [1 2] [3 4] [5 6]))
[1 2 3 4 5 6]

↪ (raze (list (list 1 2) (list 3 4)))
[1 2 3 4]

↪ (raze (list 'a' "bc" 'd'))
"abcd"
```

## Mixed Types

When elements have different types, the result is a heterogeneous list:

```clj
↪ (raze (list [1 2] [3.0 4.0]))
(
  1
  2
  3.00
  4.00
)

↪ (raze (list (list 1 2) (list 3.0 4)))
(
  1
  2
  3.00
  4
)
```

## Edge Cases

```clj
↪ (raze (list [1 2 3]))
[1 2 3]

↪ (raze (list))
↪ (show (raze (list)))
Null

↪ (raze 42)
42
```

!!! info
    - **Input**: A list containing vectors or lists
    - **Output**: A flattened vector (if all elements have the same type) or list (if types differ)
    - When all input elements are vectors of the same type, `raze` uses an optimized memcpy path
    - Non-list inputs are returned unchanged
    - Similar to kdb+'s `raze` function

!!! tip
    Use `raze` to flatten results from parallel operations like `pmap`, or to merge multiple arrays into one
