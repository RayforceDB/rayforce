# :simple-googlebigquery: Select Query

The `select` function performs data selection, filtering, and aggregation on [:material-table: Tables](../data-types/table.md). `select` operations are automatically optimized and parallelize aggregations when possible. Grouping and filtering are highly efficient on columnar data.

```clj
(set employees (table [name dept salary hire_date] 
  (list 
    (list "Alice" "Bob" "Charlie" "David") 
    ['IT 'HR 'IT 'IT] 
    [75000 65000 85000 72000] 
    [2021.01.15 2020.03.20 2019.11.30 2022.05.10])))

;; Select employees with salary > 70000, grouped by department
(select {
  avg_salary: (avg salary)
  headcount: (count name)
  from: employees
  where: (> salary 70000)
  by: dept})
┌──────┬────────────┬───────────┐
│ dept │ avg_salary │ headcount │
├──────┼────────────┼───────────┤
│ IT   │ 77333.33   │ 3         │
├──────┴────────────┴───────────┤
│ 1 rows (1 shown) 3 columns    │
└───────────────────────────────┘
```

!!! note ""
    All `select` parameters are provided as a single [:material-code-braces: Dictionary](../data-types/dictionary.md). The `from` clause is **required**.


!!! warning "Column Name Conflicts"
    If a column name matches a built-in [:material-function: function](../data-types/functions.md) or environment variable, use `(at table 'column)` to access it:
    
    ```clj
    (select {
      name: (at employees 'name)
      from: employees})
    ```

## Filtering with `where`

Use the `where` clause to filter rows based on conditions. The `where` clause accepts any boolean expression that evaluates to a [:material-vector-line: Vector](../data-types/vector.md) of [:simple-scalar: Booleans](../data-types/boolean.md) or a single boolean value.

```clj
;; Select employees with salary greater than 70000
(select {
  name: name 
  salary: salary 
  from: employees 
  where: (> salary 70000)})
┌─────────┬────────┐
│ name    │ salary │
├─────────┼────────┤
│ Alice   │ 75000  │
│ Charlie │ 85000  │
│ David   │ 72000  │
└─────────┴────────┘
```

You can use complex conditions with logical operators:

```clj
;; Select IT employees with salary between 70000 and 80000
(select {
  name: name 
  salary: salary 
  from: employees 
  where: (and (= dept 'IT) (>= salary 70000) (<= salary 80000))})
```

## Aggregation

Use [Aggregations](../operations/math.md) to compute columns:

```clj
(select {
  total_salary: (sum salary) 
  avg_salary: (avg salary) 
  headcount: (count name) 
  from: employees})
┌──────────────┬────────────┬───────────┐
│ total_salary │ avg_salary │ headcount │
├──────────────┼────────────┼───────────┤
│ 297000       │ 74250.00   │ 4         │
└──────────────┴────────────┴───────────┘
```


## Grouping with `by`

Group rows using the `by` keyword to perform aggregations per group. The `by` clause accepts a [:simple-scalar: Symbol](../data-types/symbol.md) or a [:material-code-braces: Dictionary](../data-types/dictionary.md) of symbols.

### Single Column Grouping

```clj
;; Group by department and calculate statistics
(select {
  avg_salary: (avg salary) 
  headcount: (count name) 
  earliest_hire: (min hire_date) 
  from: employees 
  by: dept})
┌──────┬────────────┬───────────┬───────────────┐
│ dept │ avg_salary │ headcount │ earliest_hire │
├──────┼────────────┼───────────┼───────────────┤
│ IT   │ 77333.33   │ 3         │ 2019.11.30    │
│ HR   │ 65000.00   │ 1         │ 2020.03.20    │
└──────┴────────────┴───────────┴───────────────┘
```

### Multiple Column Grouping

Group by multiple columns using a [:material-code-braces: Dictionary](../data-types/dictionary.md):

```clj
;; Group by both department and region
(select {
  dept: dept 
  region: region 
  avg_salary: (avg salary) 
  from: employees 
  by: {dept: dept region: region}})
```
