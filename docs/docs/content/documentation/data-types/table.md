# :material-table: Table

!!! note ""
    Type Code: `98`. Internal Name: `Table`

A Table is an object that consists of columns, forming the core structure for relational operations in RayforceDB.

## Structure

**Column headers** are a [:material-vector-line: Vector](./vector.md) of [:simple-scalar: Symbols](./symbol.md), and they serve as the names of the columns.

**Column values** are [:material-vector-line: Vectors](./vector.md) or [:material-code-array: Lists](./list.md) of any type.

```clj
(table [id name age] (list ['001 '002 '003] (list "Alice" "Bob" "Charlie") [20 30 40]))
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

## Table Storage and Loading

Tables can be stored and loaded from disk in different formats for efficient access and persistence.

### :material-database-import: Get Splayed

Loads a splayed table from disk. A splayed table stores each column as a separate file for efficient columnar access.

```clj
(get-splayed "/tmp/db/tab/" "/tmp/db/tab.sym")
```

The second argument (symfile path) is optional. If not provided, the symfile will be inferred from the table path.

!!! note ""
    Pay attention to the trailing slash in the path to the table. It is required.

### :material-database-export: Set Splayed

Stores a [:material-table: Table](../data-types/table.md) as a splayed table on disk. Each column is saved as a separate file.

```clj
(set-splayed "/tmp/db/tab/" t)
(set-splayed "/tmp/db/tab/" t "/tmp/db/sym")
```

Optionally accepts a string path to a symfile. If provided, symbol columns will use this shared symfile.

!!! note ""
    The table path must end with a trailing slash to indicate it's a directory.

### :material-database-import: Get Parted

Loads a parted table from disk. A parted table is organized by partitions.

```clj
(get-parted "/tmp/db/" 'tab)
```

Takes two arguments: the root path to the parted tables directory and a symbol representing the table name.

### :material-database-export: Set Parted

Stores a [:material-table: Table](../data-types/table.md) as a parted table on disk, organized by partitions.

```clj
(set-parted "/tmp/db/" t)
```

### :material-file-document-outline: Read CSV

Reads a CSV file and converts it into a [:material-table: Table](../data-types/table.md).

```clj
(read-csv [I64 Symbol F64] "/path/to/file.csv")
(read-csv [I64 Symbol F64] "/path/to/file.csv" ';')
```

Takes a vector of type symbols specifying the column types, a file path, and optionally a separator character (defaults to comma).

### :material-arrow-right: Next: Table [:simple-googlebigquery: Queries](../queries/overview.md)
