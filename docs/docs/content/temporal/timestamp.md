# Timestamp

Represents a point in time combining date and time.

```clj
;; Create timestamp from string
↪ (as 'timestamp "2024.03.15D10:30:00")
2024.03.15D10:30:00.000000000
```

```clj
;; Get current timestamp (returns current timestamp)
(timestamp 'local)
(timestamp 'utc)

;; Timestamp arithmetic and comparisons use D separator
(+ 2024.03.15D10:30:00 3600000000000)
(< 2024.03.15D10:30:00 2024.03.15D11:30:00)
(as 'date 2024.03.15D10:30:00)
(as 'time 2024.03.15D10:30:00)
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
