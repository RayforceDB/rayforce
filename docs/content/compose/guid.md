# Guid `guid`

Generates a list of unique identifiers.

```clj
↪ (guid 2)
[f162feeb-9a43-4168-9a5b-0bb362361a22 58a0c91a-6ece-4df0-8b1d-edc47553017f]
```

!!! info
    - Takes an i64 number as input specifying how many GUIDs to generate
    - Returns a vector of unique GUIDs
    - Each GUID is guaranteed to be unique

!!! tip
    Useful for generating unique identifiers for database records or distributed systems
