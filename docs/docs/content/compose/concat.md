# Concat `concat`

Concatenates two values into a single list or array.

## Basic Usage

```clj
↪ (concat 1 2)
[1 2]

↪ (concat 1 [2 3])
[1 2 3]

↪ (concat [1 2] 3)
[1 2 3]

↪ (concat (list 1 2 "asd") 7)
(
1
2
asd
7
)
```

## Type-Specific Examples

```clj
↪ (concat true false)
[true false]
```

```clj
↪ (concat 0x0d 0x0a)
[0x0d 0x0a]

↪ (concat 't' "est")
"test"

↪ (concat "te" "st")
"test"

↪ (concat 1h 2h)
[1 2]

↪ (concat 1i 2i)
[1 2]
```

!!! info
    - **Supported types**: boolean, byte, char, string, int16, int32, int64, float64, date, time, timestamp, symbol, guid
    - When concatenating arrays of the same type, the result is an array
    - When concatenating different types, the result is a list
    - Single values are treated as one-element lists/arrays
    - Strings: `char + string`, `string + char`, or `string + string` produces a string
    - Special handling for null terminators in strings (`\000` is trimmed)

!!! tip
    Use `concat` to build arrays incrementally, combine heterogeneous data, or construct strings from characters
