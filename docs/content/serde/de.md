# Deserialize `de`

Deserializes a binary format back into a value.

```clj
↪ (de (ser 42))
42

↪ (de (ser "hello"))
"hello"

↪ (de (ser [1 2 3]))
[1 2 3]

↪ (de (ser {a: 1 b: 2}))
{
  a: 1
  b: 2
}
```

!!! info
    - Reconstructs values from serialized byte arrays
    - Preserves original types and structures
    - Can deserialize any value that was serialized with ser

!!! warning
    - Ensure the serialized data is valid and complete
    - When deserializing functions, required dependencies must be available

!!! tip
    Use de to recover data that was previously serialized with ser
