# Dict `dict`

The `dict` function creates a dictionary with the specified keys and values.

```clj
↪ (dict [a b c] [1 2 3])
{
  a: 1
  b: 2
  c: 3
}
↪ (dict [a b c] (list "A" [1 2] 5.77))
{
  a: A
  b: [1 2]
  c: 5.77
}
```
