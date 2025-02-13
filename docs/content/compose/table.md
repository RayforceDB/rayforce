# Table `table`

Creates a table from column names and values.

```clj
↪ (table [A B C] (list (til 5) [6 7 8 9 0] (list "A" "B" "C" "D" "E")))
┌────┬────┬──────────────────────────────┐
│ A  │ B  │ C                            │
├────┼────┼──────────────────────────────┤
│ 0  │ 6  │ A                            │
│ 1  │ 7  │ B                            │
│ 2  │ 8  │ C                            │
│ 3  │ 9  │ D                            │
│ 4  │ 0  │ E                            │
├────┴────┴──────────────────────────────┤
│ 5 rows (5 shown) 3 columns (3 shown)   │
└────────────────────────────────────────┘
```

!!! info
    - First argument: vector of column names (symbols)
    - Second argument: list of column values
    - All columns must have the same length

!!! warning
    Column names must be unique and values must have matching lengths

!!! tip
    Tables are useful for organizing and analyzing structured data