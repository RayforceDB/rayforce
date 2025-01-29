
# Sub `-`

Makes a subtraction of it's arguments. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`, `Timestamp`.

``` clj
↪ (- 1 2)
-1
↪ (- 1.2 2.2)
-1
↪ (- 1 3.4)
-2
↪ (- [1 2 3] 3)
[-2 -1 0]
↪ (- [1 2 3] 3.1)
[-2.1 -1.1 -0.1]
↪ (- 3.1 [1 2 3])
[2.1 1.1 0.1]
```
