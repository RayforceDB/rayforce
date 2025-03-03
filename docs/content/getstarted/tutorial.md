# :material-school: Tutorial

## Getting Started

### Basic Operations

Let's start with some basic arithmetic operations:

```clj
;; Basic arithmetic
↪ (+ 1 2)
3

;; Multiple operations
↪ (* (+ 2 3) 4)
20

;; Integer part of division
↪ (/ -10 3)
-3

;; Remainder from division
↪ (% 5 -3)
-2

;; Division with decimals
↪ (/ 10 3)
3.33
```

### Variables

Variables are created using `set`:

```clj
;; Assign a value
↪ (set x 42)
42

;; Use the variable
↪ (+ x 10)
52

;; Multiple assignments
↪ (set y (+ x 5))
47
```

## Working with Collections

### Vectors

Vectors are homogeneous collections (all elements must be the same type):

```clj
;; Create a vector of numbers
↪ (set numbers [1 2 3 4 5])
[1 2 3 4 5]

;; Vector operations
↪ (* numbers 2)  ;; Multiply each element by 2
[2 4 6 8 10]

;; Aggregations
↪ (avg numbers)
3.00
↪ (sum numbers)
15
```

### Lists and Strings

Lists can contain mixed types, and strings must be created using lists:

```clj
;; Create a list of strings
↪ (set names (list "Alice" "Bob" "Charlie"))
(
  Alice
  Bob
  Charlie
)


;; Mixed type list
↪ (set data (list 1 "two" [3 4 5]))
(
  1
  two
  [3 4 5]
)
```

## Working with Tables

Tables are the primary data structure for data analysis:

```clj
;; Create a simple table
↪ (set employees (table [name dept salary]
                       (list (list "Alice" "Bob" "Charlie")
                            ['IT 'HR 'IT]
                            [75000 65000 85000])))

;; Display the table
↪ employees
┌─────────┬──────┬─────────────────────┐
│ name    │ dept │ salary              │
├─────────┼──────┼─────────────────────┤
│ Alice   │ IT   │ 75000               │
│ Bob     │ HR   │ 65000               │
│ Charlie │ IT   │ 85000               │
├─────────┴──────┴─────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown) │
└──────────────────────────────────────┘

;; Query the table
↪ (select {name: name
           avg_salary: (avg salary)
           from: employees
           by: dept})
┌──────┬─────────────────┬─────────────────────┐
│ dept │ name            │ avg_salary          │
├──────┼─────────────────┼─────────────────────┤
│ IT   │ (Alice Charlie) │ 80000.00            │
│ HR   │ (Bob)           │ 65000.00            │
├──────┴─────────────────┴─────────────────────┤
│ 2 rows (2 shown) 3 columns (3 shown)         │
└──────────────────────────────────────────────┘

```

## Working with Time

Rayforce has built-in support for temporal data:

```clj
;; Get current timestamp
↪ (set now (timestamp 'local))
2025.03.03D16:41:57.668734712

;; Extract components
;; TODO
↪ (as 'date now)
2025.03.03
↪ (as 'time now)
16:41:57.668

;; Date arithmetic
↪ (+ 2024.03.15 7)  ;; Add 7 days
2024.03.22
```

## Functions

Create reusable code with functions:

```clj
;; Define a simple function
↪ (set double (fn [x] (* x 2)))
↪ (double 21)
42

;; Function with multiple arguments
↪ (set salary-increase (fn [amount pct]
                         (+ amount (* amount (div pct 100)))))
↪ (salary-increase 50000 10)  ;; 10% increase
55000

;; Function with multiple expressions
↪ (set process-data (fn [data]
                      (set doubled (* data 2))
                      (set summed (sum doubled))
                      (/ summed (count data))))
↪ (process-data [1 2 3 4 5])
6
```

## Error Handling

Handle errors gracefully with try/catch:

```clj
;; Basic error handling
↪ (try
     (+ 1 'symbol)
     (fn [x] x))
"add: unsupported types: 'i64, 'symbol"

;; Error handling in functions
↪ (set safe-divide (fn [x y]
                     (try
                       (+ x 'invalid)
                       (fn [msg] 
                           (list "Error:" msg)))))
@safe-divide              
↪ (safe-divide 10 null)
(
  Error:
  add: unsupported types: 'i64, 'symbol
)
↪
```

## Working with Files

Load and save data:

```clj
;; Save string to file
↪ (set "calculations.rf" "(+ 1 2)")

;; Load and execute code
↪ (eval (get "calculations.rf"))
3
```

## Performance Tips

### Profiling Code

Use `timeit` to measure performance:

```clj
;; Measure execution time
↪ (timeit (sum [1 2 3 4 5]))
0.015  ;; Time in milliseconds

↪ (timeit (set t (load "./examples/table.rfl")))
2544.45

↪ :t 1
. Timeit is on.
↪ (select {from: t by: Timestamp p: (first OrderId) s: (sum Price)})
┌───────────────────────────────┬──────────────────────────────────────┬──────────────┐
│ Timestamp                     │ p                                    │ s            │
├───────────────────────────────┼──────────────────────────────────────┼──────────────┤
│ 2000.01.01D00:00:00.000000000 │ 8f8e1595-9a9b-40b9-94aa-313167e5ae91 │ 0.00         │
│ 2000.01.01D00:00:00.000000001 │ 8a254305-a53a-4275-912a-7c8049734fdb │ 1.00         │
│ 2000.01.01D00:00:00.000000002 │ ea2b5b2d-ffe2-45a3-b866-78f8b9a4c19a │ 2.00         │
│ 2000.01.01D00:00:00.000000003 │ 9bee0d43-dbce-436a-9211-a5ecdda227a3 │ 3.00         │
│ 2000.01.01D00:00:00.000000004 │ 467292b7-c071-4b2a-b47e-ae6dd1aad1b4 │ 4.00         │
│ 2000.01.01D00:00:00.000000005 │ 530b3339-8312-4e86-a38f-96495be57ca2 │ 5.00         │
│ 2000.01.01D00:00:00.000000006 │ 6b0d6b67-8ba6-4bdf-8ae4-a3e43561f1f4 │ 6.00         │
│ 2000.01.01D00:00:00.000000007 │ 8de8050f-f5fc-4fe8-b1b9-76101834ea59 │ 7.00         │
│ 2000.01.01D00:00:00.000000008 │ 6d1f4b29-491f-49d0-917c-72f2b90866e0 │ 8.00         │
│ 2000.01.01D00:00:00.000000009 │ 2b97d285-7eb0-49b2-b54f-87cab53e3e3c │ 9.00         │
┆ …                             ┆ …                                    ┆ …            ┆
│ 2000.01.01D00:00:00.009999990 │ b659d7a2-a542-4636-90e4-9dbc0d75f362 │ 9.999990e+06 │
│ 2000.01.01D00:00:00.009999991 │ e40317c5-ea0b-4fdb-881b-3b414307d198 │ 9.999991e+06 │
│ 2000.01.01D00:00:00.009999992 │ ed64c465-530d-4963-9fc3-7e6cacbf3444 │ 9.999992e+06 │
│ 2000.01.01D00:00:00.009999993 │ 66f43b77-ada8-476b-97cd-6e8a676b1f69 │ 9.999993e+06 │
│ 2000.01.01D00:00:00.009999994 │ c3fae9b6-3335-4de2-852a-a2632f4f239d │ 9.999994e+06 │
│ 2000.01.01D00:00:00.009999995 │ 9a852a12-6640-42d3-8413-c3121cc4297f │ 9.999995e+06 │
│ 2000.01.01D00:00:00.009999996 │ ed40dc0b-79e3-4a70-a8c2-39054f5fe35e │ 9.999996e+06 │
│ 2000.01.01D00:00:00.009999997 │ fa11edee-0f5f-41b0-adcc-c7c605730115 │ 9.999997e+06 │
│ 2000.01.01D00:00:00.009999998 │ dbfcb56e-a0f5-4e05-91f6-177571fdc471 │ 9.999998e+06 │
│ 2000.01.01D00:00:00.009999999 │ 2d33b7dc-51e3-4e43-ab66-503c3afc2523 │ 9.999999e+06 │
├───────────────────────────────┴──────────────────────────────────────┴──────────────┤
│ 10000000 rows (20 shown) 3 columns (3 shown)                                        │
└─────────────────────────────────────────────────────────────────────────────────────┘
╭ top-level
│ •  parse: 0.01 ms
│ ╭ select
│ │ •  fetch table: 0.00 ms
│ │ ╭ group
│ │ │ •  get keys: 0.00 ms
│ │ │ •  index scope: 1.56 ms
│ │ │ •  index group scoped perfect: 25.12 ms
│ │ │ •  build index: 0.00 ms
│ │ │ •  apply 'first' on group columns: 26.80 ms
│ │ ╰─┤ 53.49 ms
│ │ •  apply mappings: 77.63 ms
│ │ •  build table: 0.03 ms
│ ╰─┤ 131.19 ms
╰─┤ 131.21 ms
↪ :t 0
. Timeit is off.
```

### Memory Management

Monitor and manage memory:

```clj
;; Check memory stats
↪ (memstat)

;; Force garbage collection
↪ (gc)
```

## Next Steps

- Explore the [Reference](../reference.md) for complete function documentation
- Check the [FAQ](../faq.md) for common questions
- Join the community and share your experience 