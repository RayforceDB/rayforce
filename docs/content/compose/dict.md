# Dict `dict`

Creates a dictionary with the specified keys and values.

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

!!! warning
    The keys and values arrays must have the same length

!!! info
    - Keys can be symbols or strings
    - Values can be of any type
    - Keys must be unique
