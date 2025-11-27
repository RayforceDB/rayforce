# Type `type`

Returns the type of a value as a symbol.

```clj
↪ (type 42)
i64
↪ (type "hello")
String
↪ (type [1 2 3])
I64
↪ (type {a: 1})
Dict
↪ (type (fn [x] x))
Lambda
```

!!! info
    - Returns a symbol representing the value's type
    - Works with all built-in types
    - Useful for type checking and conditional logic

!!! tip
    Use type for runtime type checking or implementing type-specific behavior
