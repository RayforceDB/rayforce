# :material-swap-horizontal: Upsert Query

The `upsert` function updates existing rows or inserts new ones in a [:material-table: Table](../data-types/table.md) based on key column(s). If a row with the same key exists, it is **updated**. Otherwise, a new row is **inserted**.

```clj
(set employees (table [id name age] (list [1 2] ['Alice 'Bob] [25 30])))

;; Upsert: id=2 exists, so it's updated; id=3 doesn't exist, so it's inserted
(set employees (upsert employees 1 (list [2 3] ['Bob-updated 'Charlie] [30 35])))

(select {id: id name: name age: age from: employees})
┌────┬─────────────┬─────┐
│ id │ name        │ age │
├────┼─────────────┼─────┤
│ 1  │ Alice       │ 25  │
│ 2  │ Bob-updated │ 30  │
│ 3  │ Charlie     │ 35  │
├────┴─────────────┴─────┤
│ 3 rows (3 shown)       │
└────────────────────────┘
```
!!! note ""
    The `upsert` function takes three arguments: the table to upsert into, the number of key columns (starting from the first column), the data to upsert

The data can be provided as a [:material-code-array: List](../data-types/list.md), [:material-code-braces: Dictionary](../data-types/dictionary.md), or another [:material-table: Table](../data-types/table.md).

## Understanding Key Columns

The first N columns (where N = number of key columns) form the **key** used to identify rows. The key determines whether a row should be updated or inserted

```clj
(set employees (table [id name age] (list [1 2] ['Alice 'Bob] [25 30])))

(set employees (upsert employees 1 (list [2 3] ['Bob-updated 'Charlie] [30 35])))
```

!!! note ""
    In this example, `id` is the key column (key_count = `1`). The row with `id=2` is **updated**, and a new row with `id=3` is **inserted**.

## Upserting a Single Row

```clj
(set employees (upsert employees 1 (list 4 'David 40)))
```

## Upserting Multiple Rows

Upsert multiple rows using a list of [:material-vector-line: Vectors](../data-types/vector.md):

```clj
(set employees (upsert employees 1 (list [5 6] ['Eve 'Frank] [28 32])))
```

## Upserting with Dictionary

Upsert rows using a [:material-code-braces: Dictionary](../data-types/dictionary.md) for clearer column mapping:

```clj
(set employees (upsert employees 1 (dict [id name age] (list 7 'Grace 29))))
```

## Upserting from Another Table

Upsert rows from another [:material-table: Table](../data-types/table.md):

```clj
(set new_data (table [id name age] (list [8 9] ['Henry 'Ivy] [45 38])))

(set employees (upsert employees 1 new_data))
```

## In-Place Upsertion

To modify the table in place without reassigning, pass the table name as a quoted [:simple-scalar: Symbol](../data-types/symbol.md):

```clj
(upsert 'employees 1 (list 11 'Kate 27))
```

!!! warning "Important: Persisting Changes"
    By default, `upsert` returns a **new table** and does not modify the original. To persist changes, you have two options:
    
    1. **Reassign the result:**
       ```clj
       (set employees (upsert employees 1 (list 11 'Kate 27)))
       ```
    
    2. **Use in-place upsertion with quoted symbol:**
       ```clj
       (upsert 'employees 1 (list 11 'Kate 27))  ;; Modifies table directly
       ```
