# Upsert `upsert`

Updates existing rows or inserts new ones based on key column(s).

```clj
;; Create table with id as key column (first column)
(set ups_t (table [id name] (list [1 2] (list "Alice" "Bob"))))
┌────┬─────────────────────────────────┐
│ id │ name                            │
├────┼─────────────────────────────────┤
│ 1  │ Alice                           │
│ 2  │ Bob                             │
├────┴─────────────────────────────────┤
│ 2 rows (2 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘


;; Upsert: update id=2, insert new id=3
;; Syntax: (upsert table key_count values_list)
(set ups_t (upsert ups_t 1 (list [2 3] (list "Bob-updated" "Charlie"))))
┌────┬─────────────────────────────────┐
│ id │ name                            │
├────┼─────────────────────────────────┤
│ 1  │ Alice                           │
│ 2  │ Bob-updated                     │
│ 3  │ Charlie                         │
├────┴─────────────────────────────────┤
│ 3 rows (3 shown) 2 columns (2 shown) │
└──────────────────────────────────────┘

;; With multiple key columns (compound key)
(set ups_t2 (table [year month value] (list [2024 2024] [1 2] [100 200])))
┌──────┬───────┬───────────────────────┐
│ year │ month │ value                 │
├──────┼───────┼───────────────────────┤
│ 2024 │ 1     │ 100                   │
│ 2024 │ 2     │ 200                   │
├──────┴───────┴───────────────────────┤
│ 2 rows (2 shown) 3 columns (3 shown) │
└──────────────────────────────────────┘
(set ups_t2 (upsert ups_t2 2 (list [2024 2024] [2 3] [250 300])))
┌──────┬───────┬───────────────────────┐
│ year │ month │ value                 │
├──────┼───────┼───────────────────────┤
│ 2024 │ 1     │ 100                   │
│ 2024 │ 2     │ 250                   │
│ 2024 │ 3     │ 300                   │
├──────┴───────┴───────────────────────┤
│ 3 rows (3 shown) 3 columns (3 shown) │
└──────────────────────────────────────┘
```

!!! warning "Returns New Table"
    `upsert` returns a new table - it does NOT modify in place.
    You must reassign to persist changes:
    ```clj
    (set my_table (upsert my_table 1 values))
    ```

!!! info "Syntax"
    ```clj
    (upsert table key_count values_list)
    ```
    - `table`: Table to upsert into
    - `key_count`: Number of leading columns that form the key (integer)
    - `values_list`: Columnar list of values (same format as `insert`)

!!! tip "Key Columns"
    The first N columns (where N = key_count) are used as the key.
    If a key exists, the row is updated. Otherwise, a new row is inserted.
