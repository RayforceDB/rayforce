# :material-code-braces: Dictionary

!!! note ""
    Type Code: `99`. Internal Name: `Dict`

Dictionaries are objects that hold information about keys and their corresponding values.

## Structure

**Keys** are represented as either a [:material-vector-line: Vector](./vector.md) or a [:material-code-array: List](./list.md) of any scalar type.

**Values** are either a [:material-vector-line: Vector](./vector.md) or a [:material-code-array: List](./list.md) of any type.

```clj
↪ (dict [a b c] (list [1 2 3] [4 5 6] ['b 'c 'a]))
{
  a: [1 2 3]
  b: [4 5 6]
  c: [b c a]
}
```

!!! warning ""
    Dictionary and [:material-table: Table](./table.md) types are similar, but the main difference is in keys: a Table holds keys as a Vector of Symbols, while a Dictionary can hold keys as any Scalar value. 
