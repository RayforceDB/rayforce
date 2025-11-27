# At `at`

Returns the element at the specified index or key from a collection.

## Syntax

```clj
(at collection index)
(at collection 'key)
```

## Examples

### Vector/List Indexing

```clj
↪ (at [1 2 3 4 5] 2)
3

↪ (at [10 20 30] 0)
10

↪ (at (list "Alice" "Bob" "Charlie") 1)
"Bob"
```

### Dictionary Access

```clj
↪ (set d (dict [a b c] [1 2 3]))
↪ (at d 'b)
2

↪ (at (dict [x y] [100 200]) 'y)
200
```

### Table Column Access

```clj
↪ (set at_t (table [name age] (list (list "Alice" "Bob") [25 30])))

↪ (at at_t 'name)
(
  Alice
  Bob
)

↪ (at at_t 'age)
[25 30]
```

### Reserved Word Columns (CRITICAL)

When a table column name matches a RayFall built-in function (like `timestamp`, `type`, `count`), you **MUST** use `(at table 'column)` to access it:

```clj
;; Create table with 'timestamp' column (reserved word)
↪ (set trades (table [timestamp price amount] (list [1000 2000 3000] [100.5 101.0 99.5] [10 20 15])))

;; WRONG - timestamp is a built-in function
;; (select {ts: timestamp from: trades})  ;; ERROR!

;; CORRECT - use at with quoted symbol
↪ (select {ts: (at trades 'timestamp) from: trades})
```

!!! warning "Reserved Word Columns"
    The following column names conflict with built-in functions and require `(at table 'column)` syntax:

    - `timestamp` - temporal function
    - `type` - type inspection
    - `count` - counting function
    - `date`, `time` - temporal functions
    - `first`, `last` - item access
    - `sum`, `avg`, `min`, `max` - aggregations
    - `key`, `value` - dictionary access
    - `where`, `group` - query clauses
    - `distinct`, `reverse` - collection operations

!!! info "Parameters"
    - **collection**: Vector, list, dictionary, or table
    - **index/key**: Integer index (0-based) or quoted symbol for key access

!!! tip
    Use `at` for:
    - Accessing elements by position in vectors/lists
    - Looking up values by key in dictionaries
    - Extracting columns from tables, especially reserved-word columns
