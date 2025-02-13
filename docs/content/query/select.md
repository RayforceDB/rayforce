# Select `select`

Performs data selection and aggregation on tables or arrays.

```clj
;; Basic selection with multiple types
↪ (set employees (table [name dept salary hire_date last_login] 
                       (list (list "Alice" "Bob" "Charlie")        ;; strings in lists
                            ['IT 'HR 'IT]                          ;; symbols in vectors
                            [75000 65000 85000]                    ;; numbers in vectors
                            [2021.01.15 2020.03.20 2019.11.30]     ;; dates
                            [2024.03.15T09:30:00 2024.03.15T10:15:00 2024.03.15T11:45:00]))) ;; timestamps

;; Select specific columns
↪ (select {name: name dept: dept salary: salary from: employees})
┌─────────┬──────┬────────┐
│ name    │ dept │ salary │
├─────────┼──────┼────────┤
│ Alice   │ IT   │ 75000  │
│ Bob     │ HR   │ 65000  │
│ Charlie │ IT   │ 85000  │
└─────────┴──────┴────────┘

;; Selection with condition
↪ (select {name: name salary: salary from: employees where: (> salary 70000)})
┌─────────┬────────┐
│ name    │ salary │
├─────────┼────────┤
│ Alice   │ 75000  │
│ Charlie │ 85000  │
└─────────┴────────┘

;; Aggregation by department
↪ (select {dept: dept
           avg_salary: (avg salary)
           headcount: (count name)
           earliest_hire: (min hire_date)
           from: employees
           by: dept})
┌──────┬────────────┬───────────┬───────────────┐
│ dept │ avg_salary │ headcount │ earliest_hire │
├──────┼────────────┼───────────┼───────────────┤
│ IT   │ 80000.0    │ 2         │ 2019.11.30    │
│ HR   │ 65000.0    │ 1         │ 2020.03.20    │
└──────┴────────────┴───────────┴───────────────┘
```

!!! info "Syntax"
    ```clj
    (select {column1: expr1
             column2: expr2
             ...
             from: source
             [where: condition]
             [by: grouping]})
    ```
    - All select parameters are part of a single dictionary
    - Column definitions and clauses are key-value pairs
    - `from` is required, `where` and `by` are optional

!!! tip "Common Uses"
    - Data filtering and transformation
    - Aggregation and analysis
    - Complex data reshaping
    - Report generation

!!! warning
    - Column names must be symbols
    - Column names in the result must be unique
    - Aggregations require compatible types
    - Group by columns must be included in the result
    - String arrays must be created using lists, not vectors
    - Symbol arrays can use vector literal syntax without quotes

!!! example "Advanced Example"
    ```clj
    ;; Multiple aggregations with filtering and temporal data
    ↪ (set activity (table [user region login_time duration] 
                          (list (list "Alice" "Bob" "Charlie")
                                ['US 'EU 'US]
                                [2024.03.15T08:00:00 2024.03.15T14:30:00 2024.03.15T09:15:00]
                                [45 30 60])))
    ↪ (select {region: region
               users: (count user)
               avg_duration: (avg duration)
               last_login: (max login_time)
               from: activity
               where: (> duration 20)
               by: region})
    ┌────────┬───────┬─────────────┬─────────────────────┐
    │ region │ users │ avg_duration│ last_login          │
    ├────────┼───────┼─────────────┼─────────────────────┤
    │ US     │ 2     │ 52.5        │ 2024.03.15T09:15:00 │
    │ EU     │ 1     │ 30.0        │ 2024.03.15T14:30:00 │
    └────────┴───────┴─────────────┴─────────────────────┘
    ```
