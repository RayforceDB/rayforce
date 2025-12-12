# :simple-googlebigquery: Queries

RayforceDB provides powerful query operations for working with [:material-table: Tables](../data-types/table.md). These operations enable you to select, filter, aggregate, modify, and join data efficiently.


## Query Operations

### [:simple-googlebigquery: Select](./select.md)

!!! note ""
    Performs data selection, filtering, and aggregation on [:material-table: Tables](../data-types/table.md). Use `select` to query data, apply filters with `where`, group with `by`, and compute aggregations.

### [:material-plus-circle: Insert](./insert.md)

!!! note ""
    Adds new rows to a [:material-table: Table](../data-types/table.md). You can insert single or multiple rows using lists, dictionaries, or other tables.

### [:material-pencil: Update](./update.md)

!!! note ""
    Modifies existing columns in [:material-table: Tables](../data-types/table.md). Update specific columns based on conditions using `where` clauses and optional grouping with `by`.

### [:material-swap-horizontal: Upsert](./upsert.md)

!!! note ""
    Updates existing rows or inserts new ones based on key column(s). If a row with the same key exists, it is updated; otherwise, a new row is inserted. Perfect for maintaining unique records.

### [:material-pencil-outline: Alter](./alter.md)

!!! note ""
    Modifies elements at specific indices in [:material-vector-line: Vectors](../data-types/vector.md), [:material-code-array: Lists](../data-types/list.md), or [:material-table: Tables](../data-types/table.md) using [:material-function: functions](../data-types/functions.md).

### [:material-table-network: Joins](./joins.md)

!!! note ""
    Combine rows from two [:material-table: Tables](../data-types/table.md) based on matching column values. Join operations enable you to merge related data from different tables.

**Available Join Types:**

- **[:material-set-left-right: Left Join](./joins.md#left-join)**: Keeps all rows from the left table and matching rows from the right table. Missing matches are filled with null values.

- **[:material-border-inside: Inner Join](./joins.md#inner-join)**: Keeps only rows where join columns match in both tables. Returns the intersection of both tables.

- **[:material-window-restore: Window Join](./joins.md#window-join)**: Joins tables based on equality for all join columns except the last one, using time windows for the last column. Ideal for time-series data analysis.
