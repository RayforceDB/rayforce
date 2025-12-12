# :material-pencil: Update Query

The `update` function modifies columns in a [:material-table: Table](../data-types/table.md) using a [:material-code-braces: Dictionary](../data-types/dictionary.md) syntax similar to the [:simple-googlebigquery: Select Query](./select.md).

```clj
(set employees (table [name dept salary] 
  (list 
    (list "Alice" "Bob" "Charlie") 
    ['IT 'HR 'IT] 
    [75000 65000 85000])))

(set employees (update {
  salary: (* salary 1.1)
  from: employees
  where: (> salary 70000)}))

(select {name: name salary: salary from: employees})
┌─────────┬────────┐
│ name    │ salary │
├─────────┼────────┤
│ Alice   │ 82500  │
│ Bob     │ 65000  │
│ Charlie │ 93500  │
└─────────┴────────┘
```

!!! note ""
    All `update` parameters are provided as a single [:material-code-braces: Dictionary](../data-types/dictionary.md). The `from` clause is **required**.

!!! warning "Column Name Conflicts"
    If a column name matches a built-in [:material-function: function](../data-types/functions.md) or environment variable, use `(at table 'column)` to access it:
    
    ```clj
    (update {
      name: (at employees 'name)
      from: employees})
    ```

## Filtering with `where`

Use the `where` clause to update only rows that match a condition. The `where` clause accepts any boolean expression that evaluates to a [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Booleans](../data-types/boolean.md) or a single boolean value.

```clj
(set employees (update {
  salary: (* salary 1.1)
  from: employees
  where: (> salary 55000)}))
```

You can use complex conditions with logical operators:

```clj
(set employees (update {
  salary: (+ salary 5000)
  from: employees
  where: (and (= dept 'IT) (> salary 70000))}))
```

## Grouping with `by`

Group rows using the `by` keyword to apply updates per group. The `by` clause accepts a [:simple-scalar: Symbol](../data-types/symbol.md) or a [:material-code-braces: Dictionary](../data-types/dictionary.md) of symbols.

### Single Column Grouping

```clj
(set employees (update {
  salary: (+ salary 1000)
  from: employees
  by: dept
  where: (> salary 55000)}))
```

### Multiple Column Grouping

Group by multiple columns using a [:material-code-braces: Dictionary](../data-types/dictionary.md):

```clj
(set trades (update {
  price: (* price 1.05)
  from: trades
  by: {dept: dept region: region}}))
```

## In-Place Update

To modify the table in place without reassigning, pass the table name as a quoted [:simple-scalar: Symbol](../data-types/symbol.md) in the `from` clause:

```clj
(update {
  salary: (* salary 1.1)
  from: 'employees
  where: (> salary 55000)})
```

!!! warning "Important: Persisting Changes"
    By default, `update` returns a **new table** and does not modify the original. To persist changes, you have two options:
    
    1. **Reassign the result:**
       ```clj
       (set employees (update {... from: employees}))
       ```
    
    2. **Use in-place update with quoted symbol:**
       ```clj
       (update {... from: 'employees})  ;; Modifies table directly
       ```
