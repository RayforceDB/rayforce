# Filter `filter`

Filter a list by a bolean vector mask.

```clj
(filter [1 2 3 4 5] [true false true false true])
[1 3 5]
```