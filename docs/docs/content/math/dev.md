# Standard deviation `dev`

The `dev` function returns the standard deviation of a given list of numbers. Always returns a float value.

```clj
↪ (dev [1 2 3 4 50])
19.03

↪ (dev [1i 2i])
0.5

↪ (dev 1)
0.0

↪ (dev [1 0Nl 2])
0.5

↪ (dev [0Nf -2.0 10.0 11.0 5.0 0Nf])
5.15

↪ (dev [0Nl 1 2 3 4 50 0Nl])
19.03
```

## Notes

- Returns `0Nf` (null float) for null inputs: `(dev [0Ni])` → `0Nf`
- Null values in vectors are skipped during calculation
- Always returns float, even for integer inputs
- Single values return `0.0` (no deviation)
