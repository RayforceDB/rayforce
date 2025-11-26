
# Mod `%`

Makes a division of it's arguments, takes a remainder. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

``` clj
↪ (% 1 2)
1
↪ (% 1.2 2.2)
1.20
↪ (% 2 3.4)
2.00
↪ (% [-1 -2 -3] 3)
[2 1 0]
↪ (% [10 -10 3] 2.1)
[1.60 0.50 0.90]
↪ (% 3.1 [1 2 -3])
[0.10 1.10 -2.90]
```
