# In `in`

Checks if an element (or elements) exists in a collection. Returns a boolean or array of booleans.

**Syntax:** `(in element collection)`

```clj
↪ (in 2 [1 2 3])
true
↪ (in 5 [1 2 3])
false
↪ (in [1 2] [1 2 3 4 5])
[true true]
↪ (in "asd" "asd")
[true true true]
↪ (in 'a' "test")
false
↪ (in 'e' "test")
true
```

!!! info
    - First argument is the element(s) to search for
    - Second argument is the collection to search in
    - Returns `true` if element is found, `false` otherwise
    - When first argument is a collection, returns an array of booleans
    - Works with: integers, floats, strings, chars, symbols, dates, times, timestamps, guids

!!! warning
    The order of arguments matters: `(in element collection)`, not `(in collection element)`