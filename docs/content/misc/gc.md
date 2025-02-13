# Garbage Collect `gc`

Triggers manual garbage collection.

```clj
↪ (gc)
null
```

!!! info
    - Frees memory by collecting unused objects
    - Returns null
    - Execution time depends on heap size

!!! warning
    Manual garbage collection should be used sparingly, as the runtime automatically manages memory

!!! tip
    Use gc when you need to explicitly free memory after large operations
