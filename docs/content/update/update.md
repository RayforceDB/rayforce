# Update Function

The `update` function allows you to modify data in tables. It supports updating single or multiple columns and conditional updates using `if` expressions.

## Syntax

```lisp
(update {
    column1: expression1    ; Column modifications
    column2: expression2    ; Additional column modifications
    from: table            ; Source table
})
```

## Parameters

- Column expressions: Specify the columns to update and their new values
- `from`: (Required) The source table to update

## Type Handling and Conditional Updates

When updating columns, you must use `if` expressions for conditional updates:

```lisp
;; Integer comparisons
(update {
    volume: (if (> volume 500)    ; Compare integers with integers
               (* volume 2)
               volume)
    from: trades
})

;; Symbol comparisons
(update {
    price: (if (== sym 'AAPL)     ; Compare symbols using ==
              (* price 2.0)
              price)
    from: trades
})
```

Note: Direct float comparisons are currently not supported in conditional expressions.

## Examples

### Basic Update
Update all values in a column:
```lisp
(update {
    price: (* price 2.0)     ; Multiply all prices by 2.0
    from: trades
})
```

### Conditional Update with Integer Comparison
Update based on integer column values:
```lisp
(update {
    price: (if (> volume 1000)     ; Integer comparison
              0.0                   ; Set new price if true
              price)                ; Keep original if false
    from: trades
})
```

### Symbol-based Update
Update based on symbol values:
```lisp
(update {
    price: (if (== sym 'AAPL)      ; Symbol comparison
              (* price 2.0)         ; Double price for AAPL
              price)                ; Keep others unchanged
    from: trades
})
```

### Multiple Column Update
Update multiple columns at once:
```lisp
(update {
    price: (* price 2.0)           ; Update prices
    volume: (* volume 2)           ; Update volumes
    from: trades
})
```

## Notes

- Updates create a new table; they don't modify the original table in place
- Column names in expressions refer to the current values in those columns
- Conditional updates must use `if` expressions
- Integer comparisons work with integer columns
- Symbol comparisons work with symbol columns
- Basic arithmetic operations work with both integers and floats
- All standard functions and operators can be used in update expressions

## Common Use Cases

1. Price adjustments based on volume thresholds
2. Volume modifications based on integer conditions
3. Symbol-specific updates
4. Bulk updates across multiple columns

## See Also

- [select](../query/select.md) - For querying data
- [alter](../update/alter.md) - For in-place modifications
- [insert](../update/insert.md) - For adding new rows
