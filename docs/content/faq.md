# :material-head-question: FAQ

## General

### What is Rayforce?
Rayforce is a high-performance data processing engine with its own language called Rayfall.

### What kind of language is Rayfall?
Rayfall is a functional programming language with:
- Dynamic typing
- First-class functions
- Immutable data structures
- Built-in support for tables and time-series data
- Native support for parallel processing

## Data Types

### What are the basic data types?
- Atoms: `i64`, `f64`, `symbol`, `char`, `date`, `time`, `timestamp`, `guid`
- Vectors: Lists of atoms of the same type
- Lists: Heterogeneous collections
- Tables: Column-oriented data structures
- Dictionaries: Key-value mappings

### How are strings represented?
Strings are vectors of characters and must be created using lists, not vector literals:
```clj
;; Correct
(list "Hello" "World")

;; Incorrect
["Hello" "World"]  ;; Vectors can't contain strings
```

### How are symbols different from strings?
Symbols are interned strings - they are stored only once in memory and comparison is very fast. They're ideal for categorical data like status codes, department names, etc.
```clj
;; Symbol vector (no quotes needed)
[pending active completed]

;; Symbol creation
'pending  ;; Quote creates a symbol
```

## Tables and Queries

### How do I create a table?
```clj
(table [name age score]  ;; Column names as symbols
       (list (list "Alice" "Bob")  ;; Strings in lists
             [25 30]               ;; Numbers in vectors
             [85 90]))
```

### How do I query data?
Using the `select` function with a dictionary of parameters:
```clj
(select {name: name 
         avg_score: (avg score)
         from: students
         where: (> score 80)
         by: age})
```

## Performance

### How does Rayforce handle large datasets?
- Column-oriented storage for efficient memory use
- Automatic parallelization of operations
- Memory-mapped files for large datasets
- Optimized vector operations

### What operations are parallelized?
Most vector operations and aggregations are automatically parallelized, including:
- Mathematical operations
- Comparisons
- Aggregations (sum, avg, etc.)
- Map/filter operations

## Development

### How do I handle errors?
Using the `try` function:
```clj
(try 
  (dangerous-operation)
  {e: (println "Error:" e)})
```

### How do I profile code?
Using the `timeit` function:
```clj
(timeit (expensive-calculation))
```

### How do I load external code?
Using the `load` function:
```clj
(load "mylib.rf")
```

## Temporal Data

### How are dates and times handled?
- Dates: Days since epoch (1970.01.01)
- Times: Milliseconds since midnight
- Timestamps: Milliseconds since epoch
```clj
;; Current timestamp in local timezone
(timestamp 'local)

;; Parse date
(as 'date "2024.03.15")
```

### What timezone handling is available?
- Local timezone based on system settings
- UTC timezone available for all temporal functions
- Timezone conversion respects daylight savings

## Memory Management

### Is there garbage collection?
Yes, Rayforce includes automatic garbage collection. Manual collection can be triggered with:
```clj
(gc)
```

### How do I check memory usage?
Using the `memstat` function:
```clj
(memstat)
```
