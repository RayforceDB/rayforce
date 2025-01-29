# Value `value`

Returns the value of a key in a mappable object like a dict or table, otherwise return the value itself.

```clj
(value [1 2])
[1 2]
(value (dict [a b] [1 2]))
[1 2]
```