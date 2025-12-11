# :simple-googlebigquery: Queries

RayforceDB provides powerful query operations for working with [:material-table: Tables](../data-types/table.md). These operations enable data selection, filtering, aggregation, modification, and joining.

### [:simple-googlebigquery: Select](./select.md)

Performs data selection, filtering, and aggregation on [:material-table: Tables](../data-types/table.md)

### [:material-pencil: Update](./update.md)

Modifies columns in [:material-table: Tables](../data-types/table.md)

### [:material-plus-circle: Insert](./insert.md)

Adds new rows to [:material-table: Tables](../data-types/table.md)

### [:material-swap-horizontal: Upsert](./upsert.md)

Updates existing rows or inserts new ones based on key column(s). If a row with the same key exists, it is updated; otherwise, a new row is inserted.

### [:material-pencil-outline: Alter](./alter.md)

Modifies elements at specific indices in [:material-vector-line: Vectors](../data-types/vector.md), [:material-code-array: Lists](../data-types/list.md), or [:material-table: Tables](../data-types/table.md) using [:material-function: functions](../data-types/functions.md).

### [:material-table-network: Joins](./joins.md)

Join operations combine rows from two [:material-table: Tables](../data-types/table.md) based on matching column values:

- **[:material-set-left-right: Left Join](./joins.md#left-join)**: Keeps all rows from the left table and matching rows from the right table
- **[:material-border-inside: Inner Join](./joins.md#inner-join)**: Keeps only rows where join columns match in both tables
- **[:material-window-restore: Window Join](./joins.md#window-join)**: Joins tables based on equality for all join columns except the last one, using time windows for the last column
