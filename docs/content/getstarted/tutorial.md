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

;; Working with decimals
↪ (/ 10 3)
3.333333
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
3
↪ (sum numbers)
15
```

### Lists and Strings

Lists can contain mixed types, and strings must be created using lists:

```clj
;; Create a list of strings
↪ (set names (list "Alice" "Bob" "Charlie"))
["Alice" "Bob" "Charlie"]

;; Mixed type list
↪ (set data (list 1 "two" [3 4 5]))
[1 "two" [3 4 5]]
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
┌─────────┬──────┬────────┐
│ name    │ dept │ salary │
├─────────┼──────┼────────┤
│ Alice   │ IT   │ 75000  │
│ Bob     │ HR   │ 65000  │
│ Charlie │ IT   │ 85000  │
└─────────┴──────┴────────┘

;; Query the table
↪ (select {name: name
           avg_salary: (avg salary)
           from: employees
           by: dept})
┌──────┬────────────┐
│ dept │ avg_salary │
├──────┼────────────┤
│ IT   │ 80000      │
│ HR   │ 65000      │
└──────┴────────────┘
```

## Working with Time

Rayforce has built-in support for temporal data:

```clj
;; Get current timestamp
↪ (set now (timestamp 'local))
2024.03.15T10:30:00

;; Extract components
↪ (as 'date now)
2024.03.15
↪ (as 'time now)
10:30:00

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
                         (+ amount (* amount (/ pct 100)))))
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
"type error: '+: expected 'i64, got 'Symbol"

;; Error handling in functions
↪ (set safe-divide (fn [x y]
                     (try
                       (+ x 'invalid)
                       (fn [msg] 
                           (list "Error:" msg)))))
↪ (safe-divide 10 null)
["Error:" "type error: '+: expected 'i64, got 'Symbol"]
```

## Working with Files

Load and save data:

```clj
;; Save code to file
↪ (save "calculations.rf" "(+ 1 2)")

;; Load and execute code
↪ (load "calculations.rf")
3
```

## Performance Tips

### Profiling Code

Use `timeit` to measure performance:

```clj
;; Measure execution time
↪ (timeit (sum [1 2 3 4 5]))
0.015  ;; Time in milliseconds

;; Compare operations
↪ (timeit (do
            (set data [1 2 3 4 5])
            (map {x: (* x 2)} data)))
0.245
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