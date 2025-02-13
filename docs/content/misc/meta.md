# Meta `meta`

Returns metadata about a value.

```clj
↪ (meta [1 2 3])
{
  type: Array
  length: 3
  element_type: I64
}
↪ (meta "hello")
{
  type: String
  length: 5
}
↪ (meta (table [a b] [[1 2] [3 4]]))
{
  type: Table
  columns: [a b]
  rows: 2
}
```

!!! info
    - Returns a dictionary containing type information
    - Different types have different metadata fields
    - Useful for runtime type inspection

!!! tip
    Use meta to inspect object properties or implement generic functions
