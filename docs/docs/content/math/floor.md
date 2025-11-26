# Floor `floor`

The `floor` function returns the largest integer less than or equal to a given number. Returns a float value (except for integer inputs which remain unchanged).

```clj
↪ (floor 1.5)
1.0

↪ (floor 1.4)
1.0

↪ (floor -1.5)
-2.0

↪ (floor [1.1 2.5 -1.1])
[1.0 2.0 -2.0]

↪ (floor -5i)
-5

↪ (floor 0Nf)
0Nf
```

## Notes

- Returns float for float inputs
- Integer inputs remain as integers
- Handles null values: `0Nf` remains `0Nf`
- Works with both scalars and vectors
