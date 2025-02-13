# Count `count`

Returns the number of elements in a collection or the length of a string.

```clj
↪ (count [1 2 3 4 5])
5
↪ (count "hello")
5
↪ (count (list 1 2 "three" [4 5]))
4
↪ (count {a: 1 b: 2 c: 3})
3
```

!!! info
    - Works with arrays, lists, strings, and dictionaries
    - For dictionaries, returns the number of key-value pairs
    - For nested structures, counts only top-level elements

!!! tip
    Use count to get the size of any collection or string
