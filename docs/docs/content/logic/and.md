# And `and`

Performs a logical AND operation on its arguments. Returns `true` only if all arguments are `true`.

```clj
(and true false)
false
(and true true)
true
(and false false)
false
(and [true false true] [false true false])
[false false false]
(and [true false true] [false true false] [true false true])
[false false false]
(and [true false true] true)
[true false true]
```

!!! info
    - Works with scalar booleans and boolean arrays
    - When applied to arrays, performs element-wise AND operation
    - Supports multiple arguments (more than 2)
    - Returns `false` if any argument is `false`

!!! tip
    Use `and` to combine multiple boolean conditions or filter data with multiple criteria
