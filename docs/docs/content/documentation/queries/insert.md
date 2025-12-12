# :material-plus-circle: Insert Query

The `insert` function adds new rows to a [:material-table: Table](../data-types/table.md). It can insert a single row or multiple rows at once.

```clj
(set employees (table [name age] (list ['Alice 'Bob] [25 30])))

(set employees (insert employees (list 'Charlie 35)))  ;; Insert a single new row

(select {name: name age: age from: employees})
┌─────────┬────────────────────────────┐
│ name    │ age                        │
├─────────┼────────────────────────────┤
│ Alice   │ 25                         │
│ Bob     │ 30                         │
│ Charlie │ 35                         │
├─────────┴────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘
```

The `insert` function takes two arguments: the table to insert into and the data to insert. The data can be provided as a [:material-code-array: List](../data-types/list.md), [:material-code-braces: Dictionary](../data-types/dictionary.md), or another [:material-table: Table](../data-types/table.md).


## Inserting Multiple Rows

Insert multiple rows using a list of [:material-vector-line: Vectors](../data-types/vector.md):

```clj
(set employees (insert employees (list ['David 'Eve] [40 25])))

(select {name: name age: age from: employees})
┌─────────┬────────────────────────────┐
│ name    │ age                        │
├─────────┼────────────────────────────┤
│ Alice   │ 25                         │
│ Bob     │ 30                         │
│ Charlie │ 35                         │
│ David   │ 40                         │
│ Eve     │ 25                         │
├─────────┴────────────────────────────┤
│ 5 rows (5 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘
```

## Inserting with Dictionary

Insert rows using a [:material-code-braces: Dictionary](../data-types/dictionary.md) for clearer column mapping:

```clj
;; Insert using dictionary for explicit column mapping
(set employees (insert employees 
  (dict [name age] (list 'Frank 45))))

(select {name: name age: age from: employees})
┌─────────┬────────────────────────────┐
│ name    │ age                        │
├─────────┼────────────────────────────┤
│ Alice   │ 25                         │
│ Bob     │ 30                         │
│ Charlie │ 35                         │
│ David   │ 40                         │
│ Eve     │ 25                         │
│ Frank   │ 45                         │
├─────────┴────────────────────────────┤
│ 6 rows (6 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘
```

## Inserting from Another Table

Insert rows from another [:material-table: Table](../data-types/table.md):

```clj
(set new_employees (table [name age] (list ['Kate 'Leo] [65 70])))

(set employees (insert employees new_employees))
```

## In-Place Insertion

To modify the table in place without reassigning, pass the table name as a quoted [:simple-scalar: Symbol](../data-types/symbol.md):

```clj
(insert 'employees (list "Mike" 75))  ;; Modifies table in-place
```

!!! warning "Important: Persisting Changes"
    By default, `insert` returns a **new table** and does not modify the original. To persist changes, you have two options:
    
    1. **Reassign the result:**
       ```clj
       (set employees (insert employees (list "Mike" 75)))
       ```
    
    2. **Use in-place insertion with quoted symbol:**
       ```clj
       (insert 'employees (list "Mike" 75))  ;; Modifies table directly
       ```
