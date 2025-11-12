# Ceil `ceil`

Returns a value rounded up to the nearest integer greater than or equal to the given value. Returns a float value (except for integer inputs which remain unchanged).

```clj
↪ (ceil 1.5)
2.0

↪ (ceil 1.4)
2.0

↪ (ceil -1.5)
-1.0

↪ (ceil [1.1 2.5 -1.1])
[2.0 3.0 -1.0]

↪ (ceil -5i)
-5i

↪ (ceil 0Nf)
0Nf
```

## Notes

- Returns float for float inputs
- Integer inputs remain as integers
- Handles null values: `0Nf` remains `0Nf`
- Works with both scalars and vectors
