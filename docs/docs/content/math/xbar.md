# Xbar `xbar`

The `xbar` function rounds values down to the nearest multiple of a specified bin width. It's commonly used for bucketing or binning data into fixed-size intervals.

## Syntax

```clj
(xbar x bin)
```

- `x` - the value(s) to round
- `bin` - the bin width (the multiple to round to)

## Examples

### Basic numeric examples

```clj
↪ (xbar (- (til 10) 5) 3i)
[-6 -6 -3 -3 -3 0 0 0 3 3]

↪ (xbar (- (til 15) 5) 3)
[-6 -6 -3 -3 -3 0 0 0 3 3 3 6 6 6 9]

↪ (xbar (- (as 'F64 (til 9)) 5.0) 3.0)
[-6.0 -6.0 -3.0 -3.0 -3.0 0.0 0.0 0.0 3.0]
```

### Working with lists of mixed types

```clj
↪ (xbar (list 10i 11i 12i 13i 14i [15i] [16i] [17i] [18i])
        (list 4 4.0 [4i] [4] [4.0] 4i 4.0 [4i] [4.0]))
(list 8 8.00 [12i] [12] [12.00] [12i] [16.00] [16i] [16.00])

↪ (xbar (list [-4i] [7i] [8i] 9i) (list [4] 4.0 4 4i))
(list [-4] [4.0] [8] 8i)
```

### Date binning

```clj
↪ (xbar (list 2020.01.01 2020.01.02 2020.01.03 2020.01.04
              [2020.01.05] [2020.01.06] [2020.01.07] [2020.01.08])
        (list 2i 2 [2i] [2] 2i 2 [2i] [2]))
(list 2019.12.31 2020.01.02 [2020.01.02] [2020.01.04]
      [2020.01.04] [2020.01.06] [2020.01.06] [2020.01.08])
```

### Time binning

```clj
↪ (xbar (list 10:20:30.400 10:20:30.800 10:20:31.200
              10:20:32.000 10:20:33.500 10:20:33.900)
        (list [1000i] 1000 00:00:01.000 1000i [1000] [00:00:01.000]))
(list [10:20:30.000] 10:20:30.000 10:20:31.000
      10:20:32.000 [10:20:33.000] [10:20:33.000])
```

### Timestamp binning

```clj
↪ (xbar (list 2025.02.03D12:13:14.123456789 2025.02.03D12:13:14.123456789)
        (list [10000i] 10000i))
(list [2025.02.03D12:13:14.123450000] 2025.02.03D12:13:14.123450000)

↪ (xbar (list 2025.02.03D12:13:14.123456789 2025.02.03D12:13:14.123456789)
        (list [10000] 00:00:00.010))
(list [2025.02.03D12:13:14.123450000] 2025.02.03D12:13:14.120000000)
```

## Notes

- The function rounds **down** to the nearest multiple of the bin width
- Works with integers, floats, dates, times, and timestamps
- Both `x` and `bin` can be scalars or lists
- When both are lists, the operation is performed element-wise
