# Reference Count `rc`

Returns the reference count of an object.

```clj
↪ (set x [1 2 3])
↪ (rc x)
1
↪ (set y x)
↪ (rc x)
2
```

!!! info
    - Shows how many references point to an object
    - Useful for debugging memory management
    - Objects are freed when reference count reaches 0

!!! warning
    This is a debugging tool and should not be relied upon in production code

!!! tip
    Use rc to track object references when debugging memory issues
