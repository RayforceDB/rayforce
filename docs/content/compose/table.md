# Table `table` 

Takes two arguments: a symbol vector of the column names and a list of values and construct a table.

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