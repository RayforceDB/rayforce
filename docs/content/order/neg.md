# Negate `neg`

Returns the negative of a number or applies negation element-wise to arrays.

```clj
↪ (neg 5)
-5
↪ (neg -3)
3
↪ (neg [1 -2 3 -4])
[-1 2 -3 4]
```

!!! info
    - Works with both single numbers and arrays
    - When used with arrays, applies element-wise
    - Preserves the type (integer or float)

!!! warning
    Only works with numeric types
