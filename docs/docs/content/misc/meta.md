# Meta `meta`

Returns metadata about a value.

```clj
(meta [1 2 3])
(meta "hello")
(meta (fn [x] (* x x)))
```

!!! warning "Work in Progress"
    Meta is not yet implemented for all types. Support is being expanded.

!!! info
    - Returns a dictionary containing type information
    - Different types have different metadata fields
    - Useful for runtime type inspection

!!! tip
    Use meta to inspect object properties or implement generic functions
