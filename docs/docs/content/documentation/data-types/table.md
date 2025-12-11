# :material-table: Table

!!! note ""
    Type Code: `98`. Internal Name: `Table`

A Table is an object that consists of columns, forming the core structure for relational operations in RayforceDB.

## Structure

**Column headers** are a [:material-vector-line: Vector](./vector.md) of [:simple-scalar: Symbols](./symbol.md), and they serve as the names of the columns.

**Column values** are [:material-vector-line: Vectors](./vector.md) or [:material-code-array: Lists](./list.md) of any type.

```clj
↪ (table [id name age] (list ['001 '002 '003] (list "Alice" "Bob" "Charlie") [20 30 40]))
┌─────┬─────────┬──────────────────────┐
│ id  │ name    │ age                  │
├─────┼─────────┼──────────────────────┤
│ 001 │ Alice   │ 20                   │
│ 002 │ Bob     │ 30                   │
│ 003 │ Charlie │ 40                   │
├─────┴─────────┴──────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown) │
└──────────────────────────────────────┘
```

Tables can be splayed, parted, loaded, and unloaded from disk. See [:material-pen: IO](../operations/IO.md) for more information.

### :material-arrow-right: Next: Table [:simple-googlebigquery: Queries](../queries/overview.md)
