# As `as`

Casts a value to a specified type.

```clj
↪ (as 'i64 1.5)
1
↪ (as 'f64 1)
1.0
↪ (as 'I64 [1.2 3 4])
[1 3 4]
↪ (as 'f64 [1 2 3])
[1.0 2.0 3.0]
↪ (as 'String 1)
"1"
↪ (as 'String [1 2 3])
"[1 2 3]" 
```

!!! info
    When casting arrays, the operation is performed element-wise.

!!! warning
    Casting between incompatible types may result in runtime errors.

!!! note
    Supported type symbols: 'i64, 'f64, 'String, 'I64, 'F64
