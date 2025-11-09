# Or `or`

Performs a logical OR operation on its arguments. Returns `true` if at least one argument is `true`.

```clj
(or true false)
true
(or false true)
true
(or false false)
false
(or true true)
true
(or [true false true] [false true false])
[true true true]
(or [true false true] [false true false] [true false true])
[true true true]
(or [true false true] true)
[true true true]
```

!!! info
    - Works with scalar booleans and boolean arrays
    - When applied to arrays, performs element-wise OR operation
    - Supports multiple arguments (more than 2)
    - Returns `true` if any argument is `true`

!!! tip
    Use `or` to combine boolean conditions where at least one must be true
