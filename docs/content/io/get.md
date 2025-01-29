# Get `get`

Accepts a symbol and returns the value of the env variable named by the symbol.

```clj
↪ (set a 1)
1
↪ (get 'a)
1
```

Or load an object, stored in a disk file, into memory.

```clj
↪ (set "/tmp/vec" (til 10))
"/tmp/vec"
↪ (get "/tmp/vec")
[0 1 2 3 4 5 6 7 8 9]
```