# Serialize `ser`

Serializes a value into a binary format.

```clj
↪ (ser 42)
[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 8 1 42]

↪ (ser "hello")
[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 13 3 104 101 108 108 111]

↪ (ser [1 2 3])
[0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 27 2 0 0 0 0 0 0 0 3 1 1 1 2 1 3]
```

!!! info
    - Converts any Rayforce value into a byte array
    - Includes type information in the serialized data
    - Preserves the complete structure of complex objects

!!! warning
    Functions are serialized as their source code - ensure all dependencies are available when deserializing

!!! tip
    Use ser when you need to store data or send it over a network
