# Key `key`

Returns a key for mappable objects like dict or table, otherwise return the value itself.

```clj
↪ (key [1 2])
[1 2]
↪ (key (dict [a b] [1 2]))
[a b]
```