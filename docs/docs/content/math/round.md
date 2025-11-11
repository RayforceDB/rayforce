# Round `round`

The `round` function rounds a number to the nearest integer. If the number is halfway between two integers, it rounds away from zero. Returns a float value.

```clj
↪ (round -0.5)
-1.0

↪ (round [-1.5 -1.1 -0.0 0Nf 0.0 1.1 1.5])
[-2.0 -1.0 0.0 0Nf 0.0 1.0 2.0]

↪ (round 0Nf)
0Nf

↪ (round [])
[]
```

## Notes

- Always returns float, even though the result is a whole number
- Handles null values: `0Nf` remains `0Nf`
- For values exactly halfway (e.g., 1.5, -0.5), rounds away from zero
- Works with both scalars and vectors
