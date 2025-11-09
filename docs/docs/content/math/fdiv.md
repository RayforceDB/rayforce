
# Fdiv `fdiv`

Makes a floating-point division of its arguments. Always returns a float result. Supports such types and their combinations: `i64`, `I64`, `f64`, `F64`.

``` clj
↪ (div 1 2)
0.50
↪ (div 1.2 2.2)
0.55
↪ (div 1 3.4)
0.29
↪ (div [1 2 3] 3)
[0.33 0.67 1.00]
↪ (div [1 2 3] 2.1)
[0.48 0.95 1.43]
↪ (div 3.1 [1 2 3])
[3.10 1.55 1.03]
```
