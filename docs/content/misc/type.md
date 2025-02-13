# Type `type`

Returns the type of a value as a symbol.

```clj
↪ (type 42)
I64
↪ (type "hello")
String
↪ (type [1 2 3])
Array
↪ (type {a: 1})
Dict
↪ (type (fn [x] x))
Function
```

!!! info
    - Returns a symbol representing the value's type
    - Works with all built-in types
    - Useful for type checking and conditional logic

!!! tip
    Use type for runtime type checking or implementing type-specific behavior
