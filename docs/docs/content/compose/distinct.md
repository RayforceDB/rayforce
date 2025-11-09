# Distinct `distinct`

Returns a list of unique elements from the input, removing duplicates.

```clj
↪ (distinct [1 2 3 1 2 3 4 5 6 7 8 9 0])
[0 1 2 3 4 5 6 7 8 9]
↪ (distinct [true true])
[true]
↪ (distinct "test")
"est"
↪ (distinct [1i 0Ni 1i])
[1]
↪ (distinct ['a 'b 'ab 'aa 'a 'aa])
[a b ab aa]
```

## Supported Types

```clj
↪ (distinct [0x12 0x12 0x10])
[0x10 0x12]
↪ (distinct [2012.12.12 2012.12.12])
[2012.12.12]
↪ (distinct [10:00:00.000 20:10:10.500 10:00:00.000])
[10:00:00.000 20:10:10.500]
↪ (distinct [2024.01.01D10:00:00.000000000 2024.01.01D10:00:00.000000000])
[2024.01.01D10:00:00.000000000]
```

!!! info
    - **Supported types**: boolean, byte, char, string, int16, int32, int64, float64, date, time, timestamp, symbol, guid
    - The output is always sorted in ascending order
    - **Null values are removed** from the result
    - When applied to **strings**, removes duplicate characters (e.g., `"test"` → `"est"`)
    - Works with nested structures (lists containing arrays, etc.)
    - Maintains the type of the input (array → array, list → list)

!!! tip
    Use `distinct` to remove duplicates from data sets
