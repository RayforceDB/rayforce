# Timestamp

Represents a point in time combining date and time.

```clj
;; Get current timestamp in local timezone
↪ (timestamp 'local)
2024.03.15T10:30:00

;; Get current timestamp in UTC
↪ (timestamp 'utc)
2024.03.15T14:30:00

;; Create timestamp from string
↪ (as 'timestamp "2024.03.15T10:30:00")
2024.03.15T10:30:00

;; Timestamp arithmetic (in milliseconds)
↪ (+ 2024.03.15T10:30:00 3600000)  ;; Add one hour
2024.03.15T11:30:00

;; Compare timestamps
↪ (< 2024.03.15T10:30:00 2024.03.15T11:30:00)
true

;; Extract components
↪ (as 'date 2024.03.15T10:30:00)
2024.03.15
↪ (as 'time 2024.03.15T10:30:00)
10:30:00
```

!!! info "Format"
    - Timestamps combine date and time
    - Display format is YYYY.MM.DDThh:mm:ss
    - Internally represented as 64-bit integers (milliseconds since epoch)

!!! warning
    - Valid range is from 1970.01.01T00:00:00 to 2099.12.31T23:59:59.999
    - Invalid timestamps return null
    - Timestamp arithmetic works in milliseconds
    - Timezone conversions respect local system settings
