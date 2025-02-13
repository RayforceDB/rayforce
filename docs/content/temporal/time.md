# Time

Represents time of day.

```clj
;; Get current time in local timezone
↪ (time 'local)
10:30:00

;; Get current time in UTC
↪ (time 'utc)
14:30:00

;; Create time from numbers
↪ (as 'time 10:30:00)
10:30:00

;; Time arithmetic (in milliseconds)
↪ (+ 10:30:00 3600000)  ;; Add one hour
11:30:00

;; Compare times
↪ (< 10:30:00 11:30:00)
true

;; Extract from timestamp
↪ (as 'time 2024.03.15T10:30:00)
10:30:00
```

!!! info "Format"
    - Times are stored as milliseconds since midnight
    - Display format is HH:MM:SS
    - Internally represented as 32-bit integers

!!! warning
    - Valid range is 00:00:00 to 23:59:59.999
    - Invalid times return null
    - Time arithmetic works in milliseconds
