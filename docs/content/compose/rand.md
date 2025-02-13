# Rand `rand`

Generates random numbers within a specified range.

```clj
↪ (rand 5 10)
[7 3 6 4 9]
↪ (rand 5 100)
[23 45 67 89 12]
```

!!! info
    - First argument: number of random elements to generate
    - Second argument: maximum value (exclusive)
    - Generated numbers are integers from 0 to max-1

!!! tip
    Useful for generating test data or implementing randomized algorithms
