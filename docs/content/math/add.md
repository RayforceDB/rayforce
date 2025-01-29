
# Add `+`

Makes an addition of it's arguments. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`, `Timestamp`.

``` clj
↪ (+ 1 2)
3
↪ (+ 1.2 2.2)
3.4
↪ (+ 1 3.4)
4.4
↪ (+ [1 2 3] 3)
[4 5 6]
↪ (+ [1 2 3] 3.1)
[4.1 5.1 6.1]
↪ (+ 3.1 [1 2 3])
[4.1 5.1 6.1]
```
