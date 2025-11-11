# Average `avg`

Calculates average value of the vector. Always returns a float value.

```clj
↪ (avg [1.0 2.0 3.0])
2.0

↪ (avg [-24 12 6])
-2.0

↪ (avg [1i 2i -3i])
0.0

↪ (avg 5i)
5.0

↪ (avg [-24 12 6 0Nl])
-2.0
```

## Notes

- Returns `0Nf` (null float) for null inputs: `(avg 0Nf)` → `0Nf`
- Null values in vectors are skipped: `(avg [-24 12 6 0Nl])` → `-2.0`
- Always returns float, even for integer inputs
