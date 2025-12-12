# <img src="../assets/rayforcedb-emoji-1.svg" alt="icon" style="width: 1.2em; vertical-align: middle; margin-right: 0.3em;"> RayforceDB Documentation

RayforceDB is a high-performance columnar database that uses LISP-like syntax. It's blazing-fast, extremely efficient (see [:material-speedometer: Benchmarks](../get-started/benchmarks/overview.md)), and weighs **under 1MB**!

## Quick Example

Here's a simple example showing how to create a table and query it:

```clj
(set employees (table [name dept salary hire_date last_login] 
  (list 
    (list "Alice" "Bob" "Charlie") 
    ['IT 'HR 'IT] 
    [75000 65000 85000] 
    [2021.01.15 2020.03.20 2019.11.30] 
    [2024.03.15D09:30:00.012 2024.03.16D09:30:00.012 2024.03.17D09:30:00.012])))

(select {name: name dept: dept salary: salary from: employees})
┌─────────┬──────┬────────┐
│ name    │ dept │ salary │
├─────────┼──────┼────────┤
│ Alice   │ IT   │ 75000  │
│ Bob     │ HR   │ 65000  │
│ Charlie │ IT   │ 85000  │
└─────────┴──────┴────────┘

(select {
  avg_salary: (avg salary) 
  headcount: (count name) 
  earliest_hire: (min hire_date) 
  from: employees 
  by: dept})
┌──────┬────────────┬───────────┬───────────────┐
│ dept │ avg_salary │ headcount │ earliest_hire │
├──────┼────────────┼───────────┼───────────────┤
│ IT   │ 80000.00   │ 2         │ 2019.11.30    │
│ HR   │ 65000.00   │ 1         │ 2020.03.20    │
├──────┴────────────┴───────────┴───────────────┤
│ 2 rows (2 shown) 4 columns (4 shown)          │
└───────────────────────────────────────────────┘
```

## :material-file-document: Documentation Structure

This documentation covers everything you need to work with RayforceDB:

### :simple-databricks: [Data Types](./data-types/overview.md)
Learn about the fundamental data types: scalars (integers, floats, symbols, strings), collections (vectors, lists, tables), and more.

### :simple-googlebigquery: [Queries](./queries/overview.md)
Discover how to query and manipulate data using `select`, `insert`, `update`, `upsert`, and join operations.

### :material-calculator: [Operations](./operations/math.md)
Explore mathematical, logical, ordering, and other operations for data manipulation.

### :material-cog: [Environment & Utilities](./environment.md)
Manage variables, handle I/O operations, formatting, serialization, and more.

