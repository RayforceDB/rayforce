# Concat `concat`

Concatenates two values.

```clj
↪ (concat 1 2)
[1 2]
↪ (concat 1 [2 3])
[1 2 3]
↪ (concat [1 2] 3)
[1 2 3]
↪ (concat (list 1 2 "asd") 7)
(
  1
  2
  asd
  7
)
```
