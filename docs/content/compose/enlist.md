# Enlist `enlist`.

Enlist is a variadic function that takes n arguments and construct a list from ones, folding it into a vector if the arguments types are the same.

```clj
↪ (enlist 1 2 3)
[1 2 3]
↪ (enlist 1 2 [3 4])
(
  1
  2
  [3 4]
)
↪ (enlist 1 2 "asd")
(
  1
  2
  asd
)
```
