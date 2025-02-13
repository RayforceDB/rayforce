# Inner Join Function

The `inner-join` function combines rows from two tables based on matching values in specified columns. It returns only the rows where there are matches in both tables.

## Syntax

```lisp
(inner-join ['column] left_table right_table)  ; Single column join
(inner-join ['col1 'col2] left_table right_table)  ; Multiple column join
```

## Parameters

- First argument: A vector of quoted symbols specifying the columns to join on
- Second argument: The first (left) table in the join
- Third argument: The second (right) table in the join

## Type Handling

When joining tables:
- Columns being joined must have compatible types
- Type consistency is maintained across the join
- Column names must be quoted with single quote in the join vector (e.g., `['sym]`)

## Examples

### Basic Inner Join
Join two tables on a single column:
```lisp
;; Create sample tables
(set trades (table [sym price volume]
    (list ['AAPL 'GOOG 'MSFT]           ; Symbols
          [150.5 2500.0 300.75]         ; Prices (F64)
          [1000 500 750])))             ; Volumes (I64)

(set quotes (table [sym bid ask]
    (list ['AAPL 'GOOG 'TSLA]          ; Some matching symbols
          [150.0 2499.0 900.0]         ; Bid prices
          [151.0 2501.0 901.0])))      ; Ask prices

;; Join on the sym column
(inner-join ['sym] trades quotes)

;; Result will contain only AAPL and GOOG rows
;; as they are present in both tables
```

### Join with Multiple Columns
Join using multiple matching columns:
```lisp
(set trades (table [sym date price volume]
    (list ['AAPL 'GOOG]                ; Symbols
          [20240213 20240213]          ; Dates
          [150.5 2500.0]               ; Prices
          [1000 500])))                ; Volumes

(set quotes (table [sym date bid ask]
    (list ['AAPL 'GOOG]               ; Matching symbols
          [20240213 20240213]         ; Matching dates
          [150.0 2499.0]              ; Bid prices
          [151.0 2501.0])))           ; Ask prices

;; Join on both sym and date columns
(inner-join ['sym 'date] trades quotes)
```

## Column Naming

In the resulting table:
- The join columns appear first in the result
- Remaining columns from both tables follow
- Column names are preserved from their original tables
- Column order is deterministic but may not match the input tables

## Notes

- Only rows with matching values in both tables are included in the result
- The join operation uses hash-based matching for efficiency
- Type compatibility is checked during the join
- Multiple columns can be used for joining by including them in the join vector
- The result is a new table containing matched rows from both tables

## Common Use Cases

1. Combining trade and quote data
2. Enriching transaction data with reference data
3. Matching orders with executions
4. Analyzing related market events
5. Validating data across multiple sources

## Performance Considerations

- Hash-based joining is used for efficient matching
- Join performance depends on the size of both tables
- Multiple column joins may be slower than single column joins
- Consider table sizes when choosing join order

## See Also

- [left-join](../join/left_join.md) - For keeping all rows from the left table
- [select](../query/select.md) - For filtering and aggregating data
- [update](../update/update.md) - For modifying data
