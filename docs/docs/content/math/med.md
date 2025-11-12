# Median `med`

The median is the middle value of a dataset. It is the value that separates the higher half from the lower half of the data. Always returns a float value.

```clj
↪ (med [3 1 2])
2.0

↪ (med [3 1 2 4])
2.5

↪ (med 2i)
2.0

↪ (med -5)
-5.0

↪ (med 0Nf)
0Nf

↪ (med [])
0Nf
```

## Notes

- If the dataset has an odd number of values, the median is the middle value
- If the dataset has an even number of values, the median is the average of the two middle values
- Always returns float, even for integer inputs
- Returns `0Nf` (null float) for empty vectors or null inputs
