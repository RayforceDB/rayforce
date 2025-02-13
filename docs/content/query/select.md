# Select Function

The `select` function is used to query and analyze data from tables. It supports filtering, grouping, and aggregation operations.

## Syntax

```lisp
(select {
    from: table          ; Required: Source table
    by: column          ; Optional: Group by column(s)
    where: condition    ; Optional: Filter condition
    column: expression  ; Optional: Column calculations/aggregations
})
```

## Parameters

- `from`: (Required) The source table to query from
- `by`: (Optional) Column(s) to group by
- `where`: (Optional) Condition for filtering rows (Note: unlike `update`, `select` uses where clauses directly)
- Any additional parameters will create new columns with the specified calculations

## Type Handling

When working with conditions and calculations, maintain type consistency:
- For floating-point columns (F64), use floating-point literals (e.g., `1.0` not `1`)
- For integer columns, use integer literals
- String values must be quoted
- Symbols must be quoted with single quote (e.g., `'AAPL`)

## Examples

### Basic Select
Shows all data from a table:
```lisp
(select {from: t})
```

### Filtering
Filter rows based on a condition:
```lisp
(select {
    from: t
    where: (> price 1000.0)  ; Show only rows where price > 1000.0
})
```

### Complex Filtering
Combine multiple conditions:
```lisp
(select {
    from: t
    where: (and (> volume 500)     ; Integer comparison
               (< price 2000.0))   ; Float comparison
})
```

### Grouping
Group data by a column:
```lisp
(select {
    from: t
    by: sym
})
```

### Aggregations
Calculate aggregated values per group:
```lisp
(select {
    from: t
    by: sym
    avg_price: (avg price)    ; Calculate average price per symbol
    total_volume: (sum volume) ; Calculate total volume per symbol
})
```

### Combining Operations
You can combine grouping, filtering, and aggregations:
```lisp
(select {
    from: t
    where: (> volume 2000)    ; Filter first
    by: sym                   ; Then group
    avg_price: (avg price)    ; Then aggregate
})
```

## Aggregation Functions

The following aggregation functions are available:
- `sum`: Calculate the sum of values
- `avg`: Calculate the average value
- `min`: Find the minimum value
- `med`: Calculate the median value
- `dev`: Calculate standard deviation

## Important Differences from Update

The `select` function handles conditions differently from the `update` function:
- In `select`: Use `where` clause for filtering rows
  ```lisp
  (select {from: t where: (> price 1000.0)})
  ```
- In `update`: Use `if` expressions for conditional updates
  ```lisp
  (update {price: (if (> price 1000.0) (* price 1.1) price) from: t})
  ```

## Notes

- Column names in the table can be referenced directly in expressions
- When using grouping (`by`), any non-grouped columns will need an aggregation function
- Filtering can be applied before or after grouping using the `where` clause
- Results are returned as a new table
- Type consistency must be maintained in conditions and calculations

## Common Use Cases

1. Filtering data based on conditions
2. Grouping and aggregating data
3. Computing derived values
4. Data analysis and reporting
5. Complex data queries

## See Also

- [update](../update/update.md) - For modifying data (note: uses different syntax for conditions)
- [alter](../update/alter.md) - For in-place modifications
- [insert](../update/insert.md) - For adding new rows
