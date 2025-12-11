# :material-text: Symbol

!!! note ""
    Scalar Type Code: `-6`. Scalar Internal Name: `symbol`. Vector Type Code: `6`. Vector Internal Name: `Symbol`.

Represents an interned string.

```clj
↪ (type 'thisissymbol) ;; Scalar
symbol
↪ (type ['first 'second]) ;; Vector
Symbol
```

Symbol can be used to associate a certain value with the variable, using function [`set`](../environment.md#set)
```clj
↪ (set t (as 'B8 [true false]))
[true false]
↪ t
[true false]
```
### :material-arrow-right: See Symbol usage as [:material-table: Table](./table.md) column headers.
