# :material-pencil-outline: Alter Query

The `alter` function modifies elements at specific indices in [:material-vector-line: Vectors](../data-types/vector.md), [:material-code-array: Lists](../data-types/list.md), or [:material-table: Tables](../data-types/table.md) using a [:material-function: function](../data-types/functions.md). It applies the function to the element at the specified index with a given value.

```clj
(set prices [100 200 300])
[100 200 300]

(set prices (alter prices + 1 10))
[100 210 300]
```

!!! note ""
    The `alter` function takes 3-4 arguments:

    - The object to modify (vector, list, or table)
    - A binary [:material-function: Function](../data-types/functions.md) to apply
    - An optional index (if omitted, applies to all elements)
    - A value to use with the function


## Working with Tables

When working with [:material-table: Tables](../data-types/table.md), specify the column name as a symbol:

```clj
(set trades (table [price volume] (list [100 200] [50 60])))

(set trades (alter trades + 'price 10))  ;; Add 10 to all values in the price column

(select {price: price volume: volume from: trades})
┌───────┬────────┐
│ price │ volume │
├───────┼────────┤
│ 110   │ 50     │
│ 210   │ 60     │
└───────┴────────┘
```

## In-Place Modification

To modify the object in place without reassigning, pass the object name as a quoted [:simple-scalar: Symbol](../data-types/symbol.md):

```clj
(set prices [100 200 300])

(alter 'prices + 10)
```

!!! warning "Important: Persisting Changes"
    By default, `alter` returns a **modified copy** and does not modify the original. To persist changes, you have two options:
    
    1. **Reassign the result:**
       ```clj
       (set prices (alter prices + 10))
       ```
    
    2. **Use in-place modification with quoted symbol:**
       ```clj
       (alter 'prices + 10)  ;; Modifies prices directly
       ```
