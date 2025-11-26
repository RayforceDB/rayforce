# Date

Represents a calendar date.

```clj
;; Create dates from literals
↪ (as 'date 2024.03.15)
2024.03.15

;; Date arithmetic - add days
↪ (+ 2024.03.15 1)
2024.03.16

;; Date arithmetic - subtract dates (returns days)
↪ (- 2024.03.20 2024.03.15)
5

;; Compare dates
↪ (< 2024.03.15 2024.03.16)
true

↪ (<= 2024.03.15 2024.03.16)
true

↪ (== 2024.03.15 2024.03.15)
true
```

```clj
;; Get current date (returns current date - non-deterministic)
(date 'local)
(date 'utc)
```

!!! info "Format"
    - Dates are stored as days since epoch (1970.01.01)
    - Display format is YYYY.MM.DD
    - Internally represented as 32-bit integers

!!! warning
    - Valid range is from 1970.01.01 to 2099.12.31
    - Invalid dates return null
    - Date arithmetic works in days
