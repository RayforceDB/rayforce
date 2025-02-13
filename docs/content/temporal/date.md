# Date

Represents a calendar date.

```clj
;; Get current date in local timezone
↪ (date 'local)
2024.03.15

;; Get current date in UTC
↪ (date 'utc) 
2024.03.15

;; Create dates from numbers
↪ (as 'date 2024.03.15)
2024.03.15

;; Date arithmetic
↪ (+ 2024.03.15 1)  ;; Add one day
2024.03.16

;; Compare dates
↪ (< 2024.03.15 2024.03.16)
true

;; Extract from timestamp
↪ (as 'date 2024.03.15T10:30:00)
2024.03.15
```

!!! info "Format"
    - Dates are stored as days since epoch (1970.01.01)
    - Display format is YYYY.MM.DD
    - Internally represented as 32-bit integers

!!! warning
    - Valid range is from 1970.01.01 to 2099.12.31
    - Invalid dates return null
    - Date arithmetic works in days
