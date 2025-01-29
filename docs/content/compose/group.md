# Group `group`

Accepts a list and return a dictionary with the keys as distinct elements of the list and the values as the indexes of the elements in the list.

```clj
↪ (group [1 2 3 1 2 3 4 5 6 7 8 9 0])
{
  0: [9]
  1: [0 3]
  2: [1 4]
  3: [2 5]
  4: [6]
  5: [7]
  6: [8]
  7: [9]
  8: [10]
  9: [11]
}
```
